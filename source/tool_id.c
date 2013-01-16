/* Copyright (c) 2012 Mentor Graphics Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* The Cloud Cache requires a consistent hash, and it also requires
   a unique identity for every complete toolchain, including the
   compile driver, compiler, assembler, collect2, linker, lto-plugin
   and specs file (in case there is an external file).

   This is achieved by hashing each tool and using the hash of the
   filenames and hashes as the key. If the mtime of a tool changes
   this will trigger a recheck, but it won't change the toolchain
   identity unless the contents change.

   We could use a cumulative hash of all the tools, but it's useful
   to have the seperate hashes so we can cache them in the cloud
   independently.
 
   CS_COMPILERCHECK="none" is therefore only usable in offline
   mode. Using a command string is similarly affected.
 
   Unlike ccache, using "mtime" or "content" mode does not change the
   hashed data. However, in "content" mode the hashes will be
   verified each time.  */

#include "ccache.h"
#include "language.h"
#include "zlib/zlib.h"

#include <string.h>

enum supported_tools
{
  TOOL_UNRECOGNIZED,
  TOOL_GCC_DRIVER
};

struct tool
{
  const char *id;  /* Used as hash_delimiter and key in .tool_id file.  */
  char *path;  /* Absolute path of the tool.  */
  char *hash;  /* MD4 "hash-size" string.  */

  struct tool *next;
};

struct tool_hashes
{
  struct tool *tools, *last;

  char *version;
  char *config;
  char *specs_hash;		/* The specs does not need a path.  */
};

/* We cache the computed hashes here to avoid duplicated effort.  */
static struct tool_hashes toolchain_hashes;
static char *toolchain_id = NULL;
static char **compiler_discovery_args = NULL;

/* Open a compressed tool_id cache file.
   Return a gzFile or NULL if there was an error.  */
static gzFile
open_tool_id (const char *cache_path)
{
  int fd;
  gzFile gf = NULL;

  fd = open (cache_path, O_RDONLY | O_BINARY);
  if (fd == -1)
    {
      cc_log ("Tool identity cache file does not exist");
      return NULL;
    }
  gf = gzdopen (fd, "rb");
  if (!gf)
    {
      close (fd);
      cc_log ("gzdopen(%s) failed", cache_path);
      return NULL;
    }
  return gf;
}

/* Look at the file and try to figure out what it is.
   The aim is two-fold:
     1. Don't run "tool -print-prog-name" blindly because
        we don't know what that might do.
     2. Support multiple tools, eventually (compiler, assembler, etc.)  */
static enum supported_tools
recognize_tool_signature (const char *path)
{
  char *data = NULL;
  size_t size;
  enum supported_tools result = TOOL_UNRECOGNIZED;

  /* These magic strings have had the first character incremented by one.
     This prevents *this* binary from matching.
     Longer strings are better for string searching algorithms.  */
  char *gcc_magic_string = x_strdup (".print-prog-name=<prog>  Display the full path to compiler component <prog>");
  gcc_magic_string[0] -= 1;

  if (!read_file(path, 0, &data, &size))
    fatal ("Could not read %s", path);

  if (memmem (data, size, gcc_magic_string, strlen (gcc_magic_string)))
    {
      cc_log ("%s recognized as a gcc driver", path);
      result = TOOL_GCC_DRIVER;
    }

  free (data);
  free (gcc_magic_string);
  return result;
}

/* Parse the user arguments for anything that might affect the compiler
   search path.  Leave ARGS[1] free for -print-prog-name to be inserted.
   Additionally, the pathnames are made absolute and separate for the
   benefit of the tool_id filename hash.
   The -B and -spec options always come in arg/path pairs. If this ever
   changes tool_id_calculate will have to be adjusted.  */
static void
select_args_for_print_prog_name ()
{
  int in, out;
  char **args = malloc (sizeof (char *) * orig_args->argc * 2);

  args[0] = x_realpath (orig_args->argv[0]);
  args[1] = NULL;

  if (args[0] == NULL)
    fatal ("Couldn't find canonical path for %s", orig_args->argv[0]);

  /* The only GCC flags that should affect -print-prog-name should be '-B',
     '--specs=<file>'.
     We need to handle both "-Bdir" and "-B dir",
     and both "-specs=file" and "-specs file".  */
  for (in = 1, out = 2; in < orig_args->argc; in++)
    if (str_startswith (orig_args->argv[in], "-B"))
      {
        char *option = orig_args->argv[in];
        char *path = (orig_args->argv[in][strlen("-B")] != '\0'
                      ? &orig_args->argv[in][strlen("-B")]
                      : (in+1 < orig_args->argc ? orig_args->argv[++in] : NULL));
        if (path)
          {
            args[out++] = x_strndup (option, strlen("-B"));
            args[out++] = x_realpath (path);
          }
      }
    else if (str_startswith (orig_args->argv[in], "--specs")
             || str_startswith (orig_args->argv[in], "-specs"))
      {
        char *option = (orig_args->argv[in][1] == '-'
                        ? &orig_args->argv[in][1]
                        : orig_args->argv[in]);
        char *path = (option[strlen("-specs")] != '\0'
                      ? &option[strlen("-specs")+1]
                      : (in+1 < orig_args->argc ? orig_args->argv[++in] : NULL));
        if (path && path[0] != '\0')
          {
            args[out++] = x_strndup (option, strlen("-specs"));
            args[out++] = x_realpath (path);
          }
      }
  args[out] = NULL;
  compiler_discovery_args = args;
}

/* Call the compiler driver with OPTION and capture the
   result.  Return the program output.  Caller frees.
   If an error occurs it exits through fatal().  */
static char *
call_compiler (const char *option, bool keep_stderr)
{
  int pipefd[2];
  char *result = NULL;
  char buffer[1024];
  int size, num;
  int fderr;
  char *env_vars[] = {"LC_ALL", NULL};

  compiler_discovery_args[1] = x_strdup (option);

  if (pipe (pipefd) != 0)
    fatal ("Could not create pipe");

  if (keep_stderr)
    fderr = pipefd[1];
  else if ((fderr = creat ("/dev/null", 0666)) == -1)
    fatal ("Could not open /dev/null");

  if (execute_fd (compiler_discovery_args, NULL, pipefd[1], NULL, fderr, env_vars))
    fatal ("Failed to run %s with %s", compiler_discovery_args[0], option);

  close (fderr);
  close (pipefd[1]);

  size = 1;
  do
    {
      num = read (pipefd[0], buffer, 256);
      if (num == -1)
	fatal ("Error reading from pipe");
      else if (num == 0)
	break;			/* EOF */

      result = x_realloc (result, size + num);
      memcpy (result + size - 1, buffer, num);
      size += num;
    }
  while (1);
  if (result)
    result[size - 1] = '\0';

  free (compiler_discovery_args[1]);
  compiler_discovery_args[1] = NULL;

  return result;
}

/* Asks the compiler where tool NAME is and then searches for it on
   the path.  If found, add the tool to the list in toolchain_hashes.
   If the tool is not found, and OPTIONAL is not true, exit via fatal().  */
static void
generate_one_tool_hash (const char *id, const char *name, bool optional)
{
  struct tool *result;
  struct mdfour hash;
  char *tool_path;
  char *option = format ("-print-prog-name=%s", name);

  /* Ask the compiler where the other tools are.  */
  tool_path = call_compiler (option, false);
  strtok (tool_path, "\r\n");
  free (option);
  if (strcmp (tool_path, name) == 0)
    {
      /* The compiler does not know, or does not care.
         This means it just gets picked up from the path,
         if it exists at all.  */
      free (tool_path);
      tool_path = find_executable (name, MYNAME);

      if (tool_path == NULL)
	{
	  if (!optional)
	    fatal ("Could not locate tool '%s'", name);
	  else
	    {
	      cc_log ("Tool '%s' not present", name);
	      return;		/* Some toolchains don't need this tool.  */
	    }
	}
    }

  cc_log ("Tool '%s' is %s", name, tool_path);

  hash_start (&hash);
  hash_file (&hash, tool_path);

  result = malloc (sizeof (struct tool));
  result->id = id;
  result->path = tool_path;
  result->hash = hash_result (&hash);
  result->next = NULL;

  toolchain_hashes.last->next = result;
  toolchain_hashes.last = result;
}

/* Call the compiler with -v and --version and capture the output.
   Store the data in toolchain_hashes.  */
static void
capture_compiler_version_data (void)
{
  char *version, *config;

  version = call_compiler ("--version", false);
  strtok (version, "\r\n"); /* Trim the copyright notice. */
  toolchain_hashes.version = version;

  config = call_compiler ("-v", true);
  toolchain_hashes.config = config;
}

/* Locate the specs files used by the compiler (using '-v' data) and
   hash the files.  There is no need to hash the built-in specs as
   hashing the compiler binary will already have taken care of that.
   Store the result in toolchain_hashes.  */
static void
generate_specs_hash (void)
{
  char *str = toolchain_hashes.config;
  struct mdfour hash;
  hash_start (&hash);

  if (!str)
    fatal ("Can't read the specs files without '-v' output");

  if (str_startswith (str, "Using built-in specs."))
    {
      /* Make sure we differentiate specs files that augment the built-in
         specs from ones that replace them.  */
      hash_delimiter (&hash, "builtin");
      str += strlen ("Using built-in specs.");
    }
  while (str[0] != '\0')
    {
      while (str[0] == '\r' || str[0] == '\n')
        str++;
      if (str_startswith (str, "Reading specs from "))
        {
          char *spec_file;
          int len;

          str += strlen ("Reading specs from ");
          len = strcspn (str, "\r\n");
          spec_file = x_strndup (str, len);
          str += len;

          /* We've found a specs file; hash it.  */
          hash_delimiter (&hash, "specs_file");
          hash_file (&hash, spec_file);
          free (spec_file);
        }
      else
        /* No more specs files.  */
        break;
    }

  toolchain_hashes.specs_hash = hash_result (&hash);
}

/* Find and hash all the components of the toolchain accessed via the
   tool at PATH.
   Enter all the data into the toolchain_hashes table.
   Finally, calculate the official ID and write it to toolchain_id.  */
static void
generate_tool_hashes (const char *path)
{
  struct mdfour hash;
  enum supported_tools what_tool;
  struct tool *tool;

  if (toolchain_id)
    return;

  what_tool = recognize_tool_signature (path);

  /* Hash the primary tool.  */
  hash_start (&hash);
  hash_file (&hash, path);
  toolchain_hashes.tools = malloc (sizeof (struct tool));
  toolchain_hashes.tools->id = "primary";
  toolchain_hashes.tools->path = strdup (path);
  toolchain_hashes.tools->hash = hash_result (&hash);
  toolchain_hashes.tools->next = NULL;
  toolchain_hashes.last = toolchain_hashes.tools;

  if (what_tool == TOOL_GCC_DRIVER)
    {
      capture_compiler_version_data ();
      generate_specs_hash ();

      generate_one_tool_hash ("cc1", "cc1", false);
      generate_one_tool_hash ("cc1plus", "cc1plus", true);
      generate_one_tool_hash ("cc1obj", "cc1obj", true);
      generate_one_tool_hash ("cc1objplus", "cc1objplus", true);

      generate_one_tool_hash ("assembler", "as", false);
      generate_one_tool_hash ("collect2", "collect2", false);
      generate_one_tool_hash ("linker", "ld", false);
      generate_one_tool_hash ("lto_plugin", "lto_plugin", true);
    }

  /* Calculate the Toolchain ID.  */
  hash_start (&hash);
  for (tool = toolchain_hashes.tools; tool; tool = tool->next)
    {
      hash_delimiter (&hash, tool->id);
      hash_string (&hash, strrchr(tool->path, '/')+1);
      hash_string (&hash, tool->hash);
    }
  if (toolchain_hashes.specs_hash)
    {
      hash_delimiter (&hash, "specs");
      hash_string (&hash, toolchain_hashes.specs_hash);
    }
  toolchain_id = hash_result (&hash);

  cc_log ("Calculated Toolchain ID: %s", toolchain_id);
}

/* Create a new toold_id cache file name ID_CACHE_PATH.
   The data must be present in toolchain_hashes and toolchain_id.  */
static void
create_tool_id_file (const char *id_cache_path)
{
  int fd;
  gzFile *gf = NULL;
  char *tmp_file;
  struct tool *tool;

  if (!toolchain_id)
    fatal ("Cannot write tool_id without first having an ID.");

  tmp_file = format ("%s.tmp.%s", id_cache_path, tmp_string ());
  fd = safe_create_wronly (tmp_file);
  if (fd == -1)
    {
      cc_log ("Failed to open %s", tmp_file);
      goto error;
    }
  gf = gzdopen (fd, "wb");
  if (!gf)
    {
      cc_log ("Failed to gzdopen %s", tmp_file);
      goto error;
    }

#define PUTS(STR) \
  do { \
    if (gzputs (gf, (STR)) == -1) \
      goto error; \
  } while(0)

#define PRINTF(FMT, ...) \
  do { \
    if (gzprintf (gf, (FMT), ##__VA_ARGS__) == 0) \
      goto error; \
  } while (0)

  /* Write Magic number.  */
  PUTS ("TCID\n");

  /* Write File Format Version.  */
  PUTS ("V0\n");

  /* Write Toolchain ID */
  PRINTF ("ID\n%s\n", toolchain_id);

  /* Write each tool hash.  */
  for (tool = toolchain_hashes.tools; tool; tool = tool->next)
    PRINTF ("tool:%s\n%s\n%s\n", tool->id, tool->path, tool->hash);
  if (toolchain_hashes.specs_hash)
    PRINTF ("specs\n%s\n", toolchain_hashes.specs_hash);

#undef PUTS
#undef PRINTF

  cc_log ("Creating %s", id_cache_path);

  /* If the link fails it's probably because another process got there first.  */
  if (link (tmp_file, id_cache_path) != 0)
    {
    error:
      cc_log ("Could not create tool_id cache file. Continuing without.");
    }
  else
    {
      struct stat idstat;
      stat (id_cache_path, &idstat);
      stats_update_size (STATS_NONE, file_size (&idstat), 1);
    }

  unlink (tmp_file);

  if (gf)
    gzclose (gf);
  else if (fd != -1)
    close (fd);
  return;
}

/* Helper function: like gzgets but without any trailing EOLN chars.  */
static char *
gzgets_trimmed (gzFile gf, char *buf, int len)
{
  char *result = gzgets (gf, buf, len);
  if (result)
    strtok (buf, "\r\n");
  return result;
}

/* Calculate the unique ID for the compiler, and store it for later
   retrieval with toold_id_get().
   If REHASH is true, don't rely on mtime and size to match a cached
   identity.  */
void
tool_id_calculate (const char *path, struct stat *st, bool rehash)
{
  struct mdfour stat_hash;
  char *id_cache_name, *id_cache_path;
  gzFile gf = NULL;
  char buffer[100];
  int i;

  if (toolchain_id != NULL)
    return;

  select_args_for_print_prog_name ();

  /* The identity cache file is indexed by its path, size, and mtime,
     plus the path, size, and mtime of any specs files, and the
     path of any -B flags.  */
  hash_start (&stat_hash);
  hash_delimiter (&stat_hash, "tool_path");
  hash_string (&stat_hash, compiler_discovery_args[0]);
  hash_delimiter (&stat_hash, "tool_mtime");
  hash_int (&stat_hash, st->st_size);
  hash_int (&stat_hash, st->st_mtime);
  for (i = 2; compiler_discovery_args[i]; i+=2)
    {
      /* The options always come in arg/path pairs.  */
      char *arg = compiler_discovery_args[i];
      char *path = compiler_discovery_args[i+1];
      hash_delimiter (&stat_hash, arg);
      hash_string (&stat_hash, path);
      if (strcmp (arg, "-specs") == 0)
        {
          struct stat specstat;
          if (stat (path, &specstat) == 0)
            {
              hash_int (&stat_hash, specstat.st_size);
              hash_int (&stat_hash, specstat.st_mtime);
            }
        }
    }
  id_cache_name = hash_result (&stat_hash);
  id_cache_path = get_path_in_cache (id_cache_name, ".tool_id");

  gf = open_tool_id (id_cache_path);
  if (!gf)
    {
      /* No such file.  We've not seen this tool before.  */
      cc_log ("New tool detected: %s (size=%ld, mtime=%ld)", path,
	      st->st_size, st->st_mtime);
      generate_tool_hashes(path);
      create_tool_id_file (id_cache_path);
      return;
    }

  cc_log ("Reading toolchain id from %s", id_cache_path);

  /* Read the magic number from the file.  */
  if (gzgets_trimmed (gf, buffer, sizeof (buffer)) == NULL)
    goto error;
  if (strcmp (buffer, "TCID") != 0)
    {
      cc_log ("Magic number did not match.");
      goto error;
    }

  /* Read the file-format version.  */
  if (gzgets_trimmed (gf, buffer, sizeof (buffer)) == NULL
      || strcmp (buffer, "V0"))
    {
      cc_log ("Toolchain ID file has unknown version.");
      goto error;
    }

  /* Read the Unique ID.  */
  if (gzgets_trimmed (gf, buffer, sizeof (buffer)) == NULL
      || strcmp (buffer, "ID")
      || gzgets_trimmed (gf, buffer, sizeof (buffer)) == NULL)
    goto error;
  cc_log ("Cached Toolchain ID: %s", buffer);

  if (rehash)
    {
      /* We now know what the cached id string was.
         But the user has requested us to confirm that.  */
      cc_log ("Rehashing toolchain binaries for %s.", path);
      generate_tool_hashes (path);

      if (strcmp (buffer, toolchain_id) != 0)
	{
	  /* MISMATCH! The toolchain changed, somehow.
	     We need to regenerate the cache file.  */
	  cc_log ("Toolchain ID does NOT match.");
	  goto recreate;
	}
      else
        cc_log ("Toolchain ID matches cache.");
    } else {
        char *retval;

        /* We're not rehashing, so take the loaded ID on trust.
           We know the path, mtime and size match anyway.  */
        toolchain_id = x_strdup (buffer);

        /* Load the cached tool data from the file.  */
        while ((retval = gzgets_trimmed (gf, buffer, sizeof (buffer))) != NULL
               && str_startswith (buffer, "tool:"))
          {
            struct tool *tool = malloc (sizeof (struct tool));
            tool->id = x_strdup (buffer + strlen ("tool:"));
            if (gzgets_trimmed (gf, buffer, sizeof (buffer)) == NULL)
              goto error;
            tool->path = x_strdup (buffer);
            if (gzgets_trimmed (gf, buffer, sizeof (buffer)) == NULL)
              goto error;
            tool->hash = x_strdup (buffer);
            tool->next = NULL;

            if (toolchain_hashes.tools)
              toolchain_hashes.last->next = tool;
            else
              toolchain_hashes.tools = tool;
            toolchain_hashes.last = tool;
          }
        if (retval && strcmp (buffer, "specs"))
          {
            if (gzgets_trimmed (gf, buffer, sizeof (buffer)) == NULL)
              goto error;
            toolchain_hashes.specs_hash = x_strdup (buffer);
          }
        if (!toolchain_hashes.tools)
          goto error;
    }

  gzclose (gf);
  return;

error:
  {
    struct tool *ptr;
    free (toolchain_id);
    free (toolchain_hashes.specs_hash);
    for (ptr = toolchain_hashes.tools; ptr; ptr = ptr->next)
      free (ptr);
    toolchain_id = NULL;
    memset (&toolchain_hashes, 0, sizeof (toolchain_hashes));
  }
  cc_log ("Could not read %s", id_cache_path);
recreate:
  gzclose (gf);
  {
    struct stat oldidstat;
    stat (id_cache_path, &oldidstat);
    stats_update_size (STATS_NONE, -file_size (&oldidstat), -1);
    x_unlink (id_cache_path);
  }
  generate_tool_hashes (path);
  create_tool_id_file (id_cache_path);
}

/* Return the unique toolchain ID for the given compiler.
   Callers should not attempt to free the returned string.  */
const char *
tool_id_get(void)
{
  if (toolchain_id == NULL)
    {
      extern struct conf *conf;
      if (str_eq(conf->compiler_check, "mtime")
          || str_eq(conf->compiler_check, "content"))
        fatal ("Toolchain ID is uninitialized in toold_get_id()");
      else
        return "offline";
    }
  return toolchain_id;
}
