/* Copyright (c) 2012 Mentor Graphics Corporation
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

/* This file handles network communications
   with the Cloud Sourcery servers. */

#include "ccache.h"
#include "conf.h"
#include "cloud.h"
#include "daemon.h"
#include "hashutil.h"
#include "hashtable_itr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <json.h>
#include <sys/time.h>


#ifndef DISABLE_FORK
#define DISABLE_FORK 0
#endif

extern struct conf *conf;

/* Timing state.  */
static void do_timer ();
static enum timer_mode {
  TIMER_NONE,
  TIMER_DIRECT_MODE,
  TIMER_PREPROCESSOR_MODE,
  TIMER_CLOUD_CACHE_GET,
  TIMER_COMPILER
} timer_state = TIMER_NONE;
static struct timeval timer_start;
static struct timeval overall_start_time;

/* Run-time constants.  */
static char *user_key_header;
static char *client_id_header = "none";

/* Other static state. */
static bool forked_already = false;

struct cloud_file_list
{
  char *path;
  struct file_hash hash;
};

/* Recorded program state, for transmission.
   This data gets placed in a shared memory region (by cloud_initialize)
   to allow data to be passed back into the parent process.  */
static struct state {
  const char *exit_reason;
  int exit_status;
  enum result_type result_type;
  struct timeval overall_duration;
  bool direct_mode_tried;
  struct timeval direct_mode_duration;
  bool preprocessor_mode_tried;
  struct timeval preprocessor_mode_duration;
  bool get_tried;
  struct timeval get_duration;
  bool compile_tried;
  struct timeval compile_duration;
  char *object_file_to_push;
  char *object_path;
  char *stderr_file_to_push;
  char *cpp_hash;
  struct cloud_file_list *source_files;
  struct cloud_file_list *include_files;
  int source_count;
  int include_count;
  const char *direct_mode_autodisabled;

  /* Any strings allocated by a child process must be allocated in the shared
     memory space. We'll keep a fixed amount handy here.  */
  char *free;
  char shared_heap[3000];
} *state;

static struct state initial_state = {
  exit_reason: 		"unknown",
  exit_status: 		9999999,
  result_type:		RT_NOT_SET,
  overall_duration:     {0, 0},
  direct_mode_tried:    false,
  direct_mode_duration: {0, 0},
  preprocessor_mode_tried: false,
  preprocessor_mode_duration: {0, 0},
  get_tried:            false,
  get_duration:         {0, 0},
  compile_tried:        false,
  compile_duration: 	{0, 0},
  object_file_to_push: 	NULL,
  object_path:		NULL,
  stderr_file_to_push: 	NULL,
  cpp_hash: 		NULL,
  source_files:		NULL,
  include_files:	NULL,
  source_count:		0,
  include_count:	0,
  NULL
};

static const char const *result_type_name_table[] = {
  "unknown",
  "client cache hit (direct mode)",
  "client cache hit (preprocessor mode)",
  "cloud cache hit",
  "client compile"
};

bool
cloud_offline_mode (void)
{
  return strcmp (conf->cloud_mode, "offline") == 0;
}

/*static char *
allocate_shared_mem (size_t len)
{
  char *result;

  if (state->free + len >= (char *)state + sizeof(struct state))
    {
      errno = ENOMEM;
      return NULL;
    }

  result = state->free;
  state->free += len;
  return result;
}*/

daemon_handle
init_daemon_connection ()
{
  daemon_handle dh = connect_to_daemon();

  if (dh == -1)
    {
      cc_log("daemon connection failed; reverting to offline mode.");
      conf->cloud_mode = "offline";
      return -1;
    }

  /* Add the cloud user key to the message headers.  */
  add_daemon_header(dh, user_key_header);
  add_daemon_header(dh, client_id_header);

  return dh;
}

/* Record that the client already forked in from_cache.
   We won't need to fork again later in post_results_to_cloud. */
void
cloud_hook_fork_successful (void)
{
  forked_already = true;
}

/* This is strcmp for cloud_file_list pathnames. It's intended for qsort.  */
static int
compare_cfl (const void *a, const void *b)
{
  struct cloud_file_list * const *cfl_a = a;
  struct cloud_file_list * const *cfl_b = b;
  return strcmp (cfl_a[0]->path, cfl_b[0]->path);
}

/* Acquire and/or post statistical information to the server.
   This runs in the background to prevent build delays.  */
static void
post_results_to_cloud(void)
{
  if (cloud_offline_mode())
    {
      /* TODO: cache results for posting later?  */
      delete_stashed_files ();
      return;
    }

  /* Ensure that we actually have some results to post.  */
  /* TODO post other data to the cloud???  */
  if (!state->cpp_hash || state->exit_status == 9999999)
    {
      delete_stashed_files ();
      return;
    }

  /* Stop all the timers.  */
  do_timer (TIMER_NONE);
  if (!forked_already)
    cloud_hook_stop_overall_timer ();

  if (DISABLE_FORK || forked_already || fork() == 0)
    {
      /* Post the results in the background.  */
      daemon_handle dh;
      struct utsname host_info;
      char *url;
      const char *post_data;
      char *stderr_data;
      json_object *jobj = json_object_new_object();
      json_object *jarray, *jsubobj;
      int i;
      FILE *fd;
      char *model_name = NULL;
      int cpu_core_count = 0, cpu_thread_count = 0;
      char *memsize = NULL;

      if (!DISABLE_FORK)
	exitfn_reset();

      if (strcmp (conf->cloud_mode, "race") == 0)
	{
	  /* We ran the compiler in a race with the cloud cache. */
	  if (!DISABLE_FORK && state->result_type == RT_LOCAL_COMPILE)
	    {
	      /* The compiler won the race (or there was a cloud cache miss).
	         Wait a few seconds for the GET request to complete so
		 we can post both timings. */
	      int retry;
	      for (retry = 0; retry < 10 && !state->get_tried; retry++)
		sleep(1);
	    }
	  else if (state->result_type == RT_CLOUD_CACHE_HIT)
	    /* The cloud cache won the race. The compiler will already have
	       been killed off, but it's possible it registered a time.
	       We should ignore this as it may not be trust-worthy.  */
	    state->compile_tried = false;
	}

      /* Collect kernel data.  */
      if (uname(&host_info) != 0)
	{
	  strcpy(host_info.sysname, "unknown");
	  strcpy(host_info.nodename, "unknown");
	  strcpy(host_info.release, "unknown");
	  strcpy(host_info.version, "unknown");
	  strcpy(host_info.machine, "unknown");
	}

      /* Collect CPU data.  */
      fd = fopen("/proc/cpuinfo", "r");
      if (fd)
	{
	  while (!feof(fd))
	    {
	      char buffer[400];
	      char *retval = fgets (buffer, 400, fd);
	      if (retval && str_startswith(buffer, "model name"))
		{
		  cpu_thread_count++;
		  if (!model_name)
		    {
		      char *tmp = strchr (buffer, ':');
		      if (tmp && tmp[1] == ' ')
			{
			  strtok (buffer, "\r\n");
			  model_name = x_strdup (tmp+2);
			}
		    }
		}
	      else
		sscanf(buffer, "cpu cores : %d", &cpu_core_count);
	    }
	  fclose (fd);
	}
      if (!model_name)
	model_name = "unknown";

      /* Collect memory data.  */
      fd = fopen("/proc/meminfo", "r");
      if (fd)
	{
	  while (!feof(fd))
	    {
	      char buffer[400];
	      char *retval = fgets(buffer, 400, fd);
	      if (retval && str_startswith(buffer, "MemTotal:"))
		{
		  char *tmp = buffer + strlen ("MemTotal:");
		  tmp += strspn (tmp, " ");
		  memsize = x_strndup (tmp, strspn (tmp, "0123456789"));
		  break;
		}
	    }
	  fclose (fd);
	}
      if (!memsize)
	memsize = "unknown";
      else if (memsize[0] == '\0')
	{
	  free (memsize);
	  memsize = "unknown";
	}

      /* First add the compile info.
         I.e. everything needed to reproduce the build.  */
      json_object_object_add(jobj, "cpp_hash",
                             json_object_new_string(state->cpp_hash));
      json_object_object_add(jobj, "toolchain_id",
                             json_object_new_string(tool_id_get()));

      if (state->stderr_file_to_push
	  && (stderr_data = read_text_file(state->stderr_file_to_push, 0)))
	{
	  json_object_object_add(jobj, "stderr",
	                         json_object_new_string(stderr_data));
	  free (stderr_data);
	}
      else
	json_object_object_add(jobj, "stderr",
	                       json_object_new_string(""));

      json_object_object_add(jobj, "exit_status",
                             json_object_new_int(state->exit_status));
      json_object_object_add(jobj, "exit_reason",
                             json_object_new_string(state->exit_reason));

      /* The command line args are passed as an array.  */
      jarray = json_object_new_array();
      for (i=0; orig_args->argv[i]; i++)
	json_object_array_add(jarray,
	                      json_object_new_string(orig_args->argv[i]));
      json_object_object_add(jobj, "args", jarray);

      json_object_object_add(jobj, "cwd",
                             json_object_new_string(get_cwd()));
      json_object_object_add(jobj, "object_path",
                             json_object_new_string(state->object_path));

      /* Initially, we only pass the source_sig. This is a bandwidth saving
         measure. The server will reject the data if it doesn't recognise the
         signature, but this ought to be the rarer case.
         The source files have to be sorted before we generate the signature,
         or else there'll be more source sets than optimal.  */
      struct cloud_file_list *sorted_cfl[state->source_count
                                         + state->include_count];
      for (i = 0; i < state->source_count; i++)
	{
	  struct cloud_file_list *cfl = &state->source_files[i];
	  if (cfl->hash.hash[0] == '\0' && cfl->hash.size == 0)
	    {
	      struct mdfour hash;
	      hash_start (&hash);
	      hash_stashed_file (&hash, cfl->path);
	      hash_result_as_bytes(&hash, cfl->hash.hash);
	      cfl->hash.size = hash.totalN;
	    }
	  sorted_cfl[i] = cfl;
	}
      for (i = 0; i < state->include_count; i++)
	{
	  struct cloud_file_list *cfl = &state->include_files[i];
	  if (cfl->hash.hash[0] == '\0' && cfl->hash.size == 0)
	    {
	      struct mdfour hash;
	      hash_start (&hash);
	      hash_stashed_file (&hash, cfl->path);
	      hash_result_as_bytes(&hash, cfl->hash.hash);
	      cfl->hash.size = hash.totalN;
	    }
	  sorted_cfl[i + state->source_count] = cfl;
	}
      qsort (sorted_cfl, state->source_count + state->include_count,
             sizeof (sorted_cfl[0]), compare_cfl);
      struct mdfour source_sig;
      hash_start (&source_sig);
      for (i = 0; i < state->source_count + state->include_count; i++)
	{
	  char *sourcehash_str = format_hash_as_string(sorted_cfl[i]->hash.hash,
	                                               sorted_cfl[i]->hash.size);
	  hash_delimiter (&source_sig, "-----");
	  hash_string (&source_sig, sorted_cfl[i]->path);
	  hash_delimiter (&source_sig, "=====");
	  hash_string (&source_sig, sourcehash_str);
	  free (sourcehash_str);
	}
      char *source_sig_str = hash_result (&source_sig);
      json_object_object_add (jobj, "source_sig",
                              json_object_new_string (source_sig_str));
      free (source_sig_str);

      /* Add timing data.  */
      jarray = json_object_new_array();
      json_object_array_add(jarray,
			    json_object_new_int(state->overall_duration.tv_sec));
      json_object_array_add(jarray,
			    json_object_new_int(state->overall_duration.tv_usec));
      json_object_object_add(jobj, "overall_duration", jarray);
      if (state->direct_mode_tried)
	{
	  jarray = json_object_new_array();
	  json_object_array_add(jarray,
				json_object_new_int(state->direct_mode_duration.tv_sec));
	  json_object_array_add(jarray,
				json_object_new_int(state->direct_mode_duration.tv_usec));
	  json_object_object_add(jobj, "direct_mode_cache_duration", jarray);
	}
      if (state->preprocessor_mode_tried)
	{
	  jarray = json_object_new_array();
	  json_object_array_add(jarray,
				json_object_new_int(state->preprocessor_mode_duration.tv_sec));
	  json_object_array_add(jarray,
				json_object_new_int(state->preprocessor_mode_duration.tv_usec));
	  json_object_object_add(jobj, "preprocessor_mode_cache_duration", jarray);
	}
      if (state->get_tried)
	{
	  jarray = json_object_new_array();
	  json_object_array_add(jarray,
				json_object_new_int(state->get_duration.tv_sec));
	  json_object_array_add(jarray,
				json_object_new_int(state->get_duration.tv_usec));
	  json_object_object_add(jobj, "get_duration", jarray);
	}
      if (state->compile_tried)
	{
	  jarray = json_object_new_array();
	  json_object_array_add(jarray,
				json_object_new_int(state->compile_duration.tv_sec));
	  json_object_array_add(jarray,
				json_object_new_int(state->compile_duration.tv_usec));
	  json_object_object_add(jobj, "compile_duration", jarray);
	}

      /* Add the result type.  */
      json_object_object_add(jobj, "type",
                             json_object_new_string(
				result_type_name_table[state->result_type]));

      /* Now add the client info.
         This will go in the user's history and for data mining.  */
      json_object_object_add(jobj, "uname_sysname",
                             json_object_new_string(host_info.sysname));
      json_object_object_add(jobj, "uname_release",
                             json_object_new_string(host_info.release));
      json_object_object_add(jobj, "uname_version",
                             json_object_new_string(host_info.version));
      json_object_object_add(jobj, "uname_machine",
                             json_object_new_string(host_info.machine));
      json_object_object_add(jobj, "cpu_model",
                             json_object_new_string(model_name));
      json_object_object_add(jobj, "cpu_core_count",
                             json_object_new_int(cpu_core_count));
      json_object_object_add(jobj, "cpu_thread_count",
                             json_object_new_int(cpu_thread_count));
      json_object_object_add(jobj, "mem",
                             json_object_new_string(memsize));

      /* Add any interesting config settings.  */
      jsubobj = json_object_new_object();
      json_object_object_add(jsubobj, "base_dir",
                             json_object_new_string(conf->base_dir));
      json_object_object_add(jsubobj, "compiler_check",
                             json_object_new_string(conf->compiler_check));
      json_object_object_add(jsubobj, "compression",
                             json_object_new_boolean(conf->compression));
      json_object_object_add(jsubobj, "compression_level",
                             json_object_new_int(conf->compression_level));
      json_object_object_add(jsubobj, "direct_mode",
                             json_object_new_boolean(conf->direct_mode));
      if (state->direct_mode_autodisabled)
	json_object_object_add(jsubobj, "direct_mode_disabled_reason",
	                       json_object_new_string(state->direct_mode_autodisabled));
      json_object_object_add(jsubobj, "extra_files_to_hash",
                             json_object_new_string(conf->extra_files_to_hash));
      json_object_object_add(jsubobj, "hard_link",
                             json_object_new_boolean(conf->hard_link));
      json_object_object_add(jsubobj, "hash_dir",
                             json_object_new_boolean(conf->hash_dir));
      json_object_object_add(jsubobj, "read_only",
                             json_object_new_boolean(conf->read_only));
      json_object_object_add(jsubobj, "recache",
                             json_object_new_boolean(conf->recache));
      json_object_object_add(jsubobj, "run_second_cpp",
                             json_object_new_boolean(conf->run_second_cpp));
      json_object_object_add(jsubobj, "sloppiness",
                             json_object_new_int(conf->sloppiness));
      json_object_object_add(jsubobj, "unify",
                             json_object_new_boolean(conf->unify));
      json_object_object_add(jsubobj, "cloud_mode",
                             json_object_new_string(conf->cloud_mode));
      json_object_object_add(jsubobj, "cs_version",
                             json_object_new_string(CS_VERSION));
      json_object_object_add(jobj, "client_config", jsubobj);

      post_data = json_object_to_json_string(jobj);
      cc_log("Sending usage data: %s", post_data);

      url = format("https://%s/v1.0/cache/", conf->cloud_server);

      dh = init_daemon_connection ();

      /* Loop at most twice, probably, but if the file cache should be cleaned
         up while were here it could be more, in theory.
         The first time around the loop we post no file attachments.
         Then we post new ones as needed.
         TODO: avoid infinite loops  */
      while (1)
	{
	  json_object *jresponse = NULL;
	  json_object *value;
	  const char *result = NULL;
	  bool http_error = false;

	  set_daemon_url(dh, url);
	  add_daemon_form_data(dh, "data", post_data);

	  /* Do the network request.  */
	  request_daemon_response (dh);
	  while (1)
	    {
	      union daemon_responses *dr;
	      switch (get_daemon_response(dh, &dr))
	        {
	        case D_REQUEST_FAILED:
	          cc_log("Data could not be posted to %s", conf->cloud_server);
	          goto fail;

	        case D_RESPONSE_INCOMPLETE:
	          cc_log("Received incomplete response from the server.");
	          goto fail;

	        case D_RESPONSE_COMPLETE:
	          goto done;

	        case D_HTTP_RESULT_CODE:
	          if (dr->http_result_code != 200)
	            {
	              cc_log("Server returned error code: %d",
	                     dr->http_result_code);
	              http_error = true;
	            }
	          break;

	        case D_BODY:
	          if (jresponse)
	            {
	              /* Ignore unexpected data parts.  */
	              cc_log("WARNING: received unexpected multipart response");
	              break;
	            }

	          if (http_error)
	            {
	              cc_log("Server response: '%s'", dr->body.data);
	              goto early_fail;
	            }

		  /* Parse the response.  */
		  jresponse = json_tokener_parse(dr->body.data);
		  if (is_error(jresponse))
		    {
		      cc_log("Error: Could not parse server response as JSON.");
		      goto early_fail;
		    }
		  break;

	        case D_ATTACHMENT:
	          /* We're not expecting any! */
	          cc_log("WARNING: received unexpected multipart response");
	          cc_log("WARNING: deleting unexpected attachment");
	          x_unlink (dr->attachment.tmp_filename);
	          break;

	        default:
	          cc_log("WARNING: unknown error reading daemon response.");
	          break;
	        }
	      free(dr);
	    }

	early_fail:
	  flush_daemon_response(dh);

	fail:
	  break;

	done:

          value = json_object_object_get(jresponse, "result");
          if (value)
            result = json_object_get_string(value);
	  if (!result)
	    {
	      cc_log("Error: Server response did not contain JSON field 'result'");
	      break;
	    }
	  else if (strcmp(result, "success") == 0)
	    {
	      /* We're done.  */
	      cc_log("Data posted to %s", conf->cloud_server);
	      break;
	    }
	  else if (strcmp(result, "error") == 0)
	    {
	      /* The server reports the request was malformed, somehow.
	         The data should be a string error message.  */
	      json_object *data = json_object_object_get(jresponse, "data");
	      if (data && json_object_is_type(data, json_type_string))
		cc_log("Server reports error: '%s'",
		       json_object_get_string(data));
	      else
		cc_log("Server reports error (no message given)");
	      break;
	    }
	  else if (strcmp (result, "source list needed") == 0)
	    {
	      /* The source files are passed as an array of json pairs.  */
	      jsubobj = json_object_new_object();
	      for (i = 0; i < state->source_count; i++)
		{
		  char *sourcehash_str;
		  struct cloud_file_list *cfl = &state->source_files[i];
		  sourcehash_str = format_hash_as_string(cfl->hash.hash,
		                                         cfl->hash.size);
		  json_object_object_add(jsubobj, cfl->path,
		                         json_object_new_string(sourcehash_str));
		  free (sourcehash_str);
		}
	      for (i = 0; i < state->include_count; i++)
		{
		  char *sourcehash_str;
		  struct cloud_file_list *cfl = &state->include_files[i];
		  sourcehash_str = format_hash_as_string(cfl->hash.hash,
		                                         cfl->hash.size);
		  json_object_object_add(jsubobj, cfl->path,
		                         json_object_new_string(sourcehash_str));
		  free (sourcehash_str);
		}
	      json_object_object_add(jobj, "sources", jsubobj);
	      post_data = json_object_to_json_string(jobj);
	      cc_log("Resending with full source list: %s", post_data);
	      continue;
	    }
	  else if (strcmp (result, "files needed") == 0)
	    {
	      /* We need to upload files to the cache.  This means repeating
	         the original request, so we reuse existing curl setup, but
	         add the file uploads as attachments.  */
	      json_object *data = json_object_object_get(jresponse, "data");
	      int i, len, count;

	      if (!data || !json_object_is_type(data, json_type_array))
		{
		  cc_log("Error: Server requested file uploads, but the filenames were missing.");
		  break;
		}

	      cc_log("Server requests file uploads...");
	      len = json_object_array_length(data);
	      for (i = 0, count = 0; i < len; i++)
		{
		  json_object *entry = json_object_array_get_idx(data, i);
		  if (entry && json_object_is_type(entry, json_type_string))
		    {
		      const char *filename = json_object_get_string(entry);
		      struct stashed_file *sf;
		      if (strcmp(filename, state->object_path) == 0)
			{
			  if (!state->object_file_to_push)
			    {
			      cc_log ("Error: we don't have an object file to upload!");
			      goto bailout;
			    }
			  sf = find_stashed_file(filename);
			  add_daemon_form_attachment(dh, "object", sf, filename);
			}
		      else
			{
			  struct cloud_file_list *found = NULL;
			  int n;

			  /* Scan the files to make sure the server isn't
			     requesting bogus files!  */
			  for (n = 0; !found && n < state->source_count; n++)
			    if (strcmp (state->source_files[n].path, filename) == 0)
			      found = &state->source_files[n];
			  for (n = 0; !found && n < state->include_count; n++)
			    if (strcmp (state->include_files[n].path, filename) == 0)
			      found = &state->include_files[n];

			  if (!found)
			    {
			      cc_log ("Error: Server requested unexpected file '%s'; bailing out!",
				      filename);
			      goto bailout;
			    }

			  sf = find_stashed_file(filename);
			  assert (sf);
			  add_daemon_form_attachment(dh, "source", sf, filename);
			}
		      cc_log("...uploading file: '%s'", filename);
		      count++;
		    }
		}

	      if (count == 0)
		{
		  cc_log ("Error: no files to upload after all.");
		  break;
		}
	      else
		continue;
	    }
	  else
	    {
	      cc_log("Error posting data to %s; giving up.", conf->cloud_server);
	      break;
	    }
	}

bailout:

      close_daemon(dh);
      free((void*)post_data);
      free((void*)url);

      /* Delete the shared memory files we created earlier.  */
      // TODO Delete files on signal exit.
      delete_stashed_files ();

      /* Exit child, bypassing parent's atexit functions.  */
      if (!DISABLE_FORK && !forked_already)
	_exit(0);
    }
}

/* Queue an object file for upload.  The actual push happens in
   post_results_to_cloud so it can happen in the background.
   CACHE_FILE is duplicated, so the caller can overwrite the original.  */
void
cloud_hook_object_file (const char *cache_file)
{
  if (cloud_offline_mode())
    return;

  state->object_file_to_push = x_strdup (cache_file);
}

/* Queue a stderr file for upload.  The actual push happens in
   post_results_to_cloud so it can happen in the background.
   CACHE_FILE is duplicated, so the caller can overwrite the original.  */
void
cloud_hook_stderr_file (const char *cache_file)
{
  if (cloud_offline_mode())
    return;

  state->stderr_file_to_push = x_strdup (cache_file);
}

/* Downloads the build results from the cloud cache, if available.
   Saves the downloads in the given files and variable.
   Returns true if successful and false otherwise.  */
bool
cloud_cache_get (const char *object_file, const char *stderr_file,
                 int *exit_status)
{
  daemon_handle dh;
  char *url;
  bool retval = false;

  if (cloud_offline_mode())
   return 0;

  do_timer (TIMER_CLOUD_CACHE_GET);

  url = format("https://%s/v1.0/cache/%s-%s",
	      conf->cloud_server, state->cpp_hash, tool_id_get());

  dh = init_daemon_connection();
  set_daemon_url(dh, url);
  request_daemon_response (dh);
  while (1)
    {
      union daemon_responses *dr;
      switch (get_daemon_response (dh, &dr))
        {
        case D_REQUEST_FAILED:
        case D_RESPONSE_INCOMPLETE:
          retval = false;
          goto done;

        case D_RESPONSE_COMPLETE:
          goto done;

        case D_HTTP_RESULT_CODE:
          if (dr->http_result_code == 403)
            {
              /* The user key was no good.  */
              fprintf (stderr,
                       "cs: error: The license key provided was invalid.\n"
                       "Please login to the web site at https://www.cloudsourcery.com/cs_keys and get your valid key.\n"
                       "Continuing in offline mode ...\n");
              retval = false;
              free (dr);
              goto early_done;
            }
          else if (dr->http_result_code != 200)
            {
              retval = false;
              goto early_done;
            }
          else
            retval = true;
          break;

        case D_BODY:
          if (strstr (dr->body.headers, "?file=data"))
            {
              /* The data part contains JSON data.  */
              json_object *data = json_tokener_parse (dr->body.data);
              json_object *status = NULL;

              if (!is_error(data))
        	status = json_object_object_get (data, "exit_status");
              if (status && json_object_is_type (status, json_type_int))
        	*exit_status = json_object_get_int (status);
              else
        	{
        	  cc_log ("Warning: Server didn't return the compiler exit_status");
        	  *exit_status = 0;
        	}
              state->exit_status = *exit_status;
            }
          else
            cc_log ("WARNING: Server returned unexpected data part");
          break;

        case D_ATTACHMENT:
          if (strstr (dr->attachment.headers, "?file=object"))
            x_rename (dr->attachment.tmp_filename, object_file);
          else if(strstr (dr->attachment.headers, "?file=stderr"))
            x_rename (dr->attachment.tmp_filename, stderr_file);
          else
            {
              cc_log("WARNING: server return unexpected attachment");
              x_unlink(dr->attachment.tmp_filename);
            }
          break;

        default:
          cc_log("WARNING: unknown error reading daemon response.");
          break;
        }
      free (dr);
    }

early_done:
  flush_daemon_response (dh);

done:
  free(url);
  do_timer (TIMER_NONE);
  return retval;
}

/* Return the saved exit_status.
   When the GET request runs in a fork the parent process never sees the
   exit_status, so this function can be used to pull it from the shared
   memory; cloud_cache_get writes the exit status there for the purpose.  */
int
cloud_cache_exit_status (void)
{
  return state->exit_status;
}

/* Close any existing timer and start a new one.  */
static void
do_timer (enum timer_mode mode)
{
  struct timeval time_now, difference;

  if (cloud_offline_mode())
    return;

  gettimeofday (&time_now, NULL);
  timersub (&time_now, &timer_start, &difference);

  /* First, close off any existing timer.  */
  switch (timer_state)
    {
    case TIMER_DIRECT_MODE:
      state->direct_mode_tried = true;
      state->direct_mode_duration = difference;
      break;
    case TIMER_PREPROCESSOR_MODE:
      state->preprocessor_mode_tried = true;
      state->preprocessor_mode_duration = difference;
      break;
    case TIMER_CLOUD_CACHE_GET:
      state->get_tried = true;
      state->get_duration = difference;
      break;
    case TIMER_COMPILER:
      state->compile_tried = true;
      state->compile_duration = difference;
      break;
    case TIMER_NONE:
    default:
      break;
    }

  timer_start = time_now;
  timer_state = mode;
}

/* Timer hooks.  */
void
cloud_hook_start_overall_timer ()
{
  gettimeofday (&overall_start_time, NULL);
}
void
cloud_hook_stop_overall_timer ()
{
  struct timeval end_time;
  gettimeofday(&end_time, NULL);
  timersub (&end_time, &overall_start_time, &state->overall_duration);
}
void
cloud_hook_starting_direct_mode ()
{
  do_timer (TIMER_DIRECT_MODE);
}
void
cloud_hook_starting_preprocessor_mode ()
{
  do_timer (TIMER_PREPROCESSOR_MODE);
}
void
cloud_hook_ending_preprocessor_mode ()
{
  do_timer (TIMER_NONE);
}
void
cloud_hook_starting_compiler_execution ()
{
  do_timer (TIMER_COMPILER);
}
void
cloud_hook_ending_compiler_execution ()
{
  do_timer (TIMER_NONE);
}

/* Record the cpp_hash string.  */
void
cloud_hook_cpp_hash(const char *cpp_hash)
{
  if (cloud_offline_mode())
    return;

  state->cpp_hash = x_strdup(cpp_hash);
}

/* Record the object_path string.  */
void
cloud_hook_object_path(const char *object_path)
{
  if (cloud_offline_mode())
    return;

  state->object_path = x_strdup(object_path);
}

/* Helper function for cloud_hook_source_file and cloud_hook_include_file.  */
static void
append_to_cloud_file_list(struct cloud_file_list **listptr, int *counterptr,
                          const char *file, struct file_hash *hash)
{
  struct cloud_file_list *list;
  int count;
  const int ARRAY_INCREMENT = 16;

  if (cloud_offline_mode())
    return;

  list = *listptr;
  count = *counterptr;
  if (count % ARRAY_INCREMENT == 0)
    list = x_realloc (list, (sizeof(struct cloud_file_list)
			     * (count + ARRAY_INCREMENT)));

  list[count].path = x_strdup (file);
  if (hash)
    memcpy (&list[count].hash, hash, sizeof(struct file_hash));
  else
    {
      list[count].hash.hash[0] = '\0';
      list[count].hash.size = 0;
    }


  *counterptr = count + 1;
  *listptr = list;
}

static void
reset_cloud_file_list(struct cloud_file_list **listptr, int *counterptr)
{
  struct cloud_file_list *cfl;
  int count;
  int i;

  if (cloud_offline_mode())
    return;

  cfl = *listptr;
  count = *counterptr;
  for (i = 0; i < count; i++)
    {
      free (cfl[i].path);
    }
  free (cfl);

  *listptr = NULL;
  *counterptr = 0;
}

/* Record what top-level source files were built.  */
void
cloud_hook_source_file(const char *source_file, struct file_hash *hash)
{
  append_to_cloud_file_list(&state->source_files, &state->source_count,
                            source_file, hash);
}

/* Record what files were included in a build.  */
void
cloud_hook_include_file(const char *include_file, struct file_hash *hash)
{
  append_to_cloud_file_list(&state->include_files, &state->include_count,
                            include_file, hash);
}

/* Record a preprocessed source file.
   This is the same as cloud_hook_source_file except that the file will
   be a temporary file that needs to be saved, and then deleted
   on exit.  */
void
cloud_hook_preprocessed_file(const char *file)
{
  if (cloud_offline_mode())
    return;

  /* We're only interested in preprocessed sources if direct mode is
     disabled, either manually or automatically.  */
  if (!conf->direct_mode)
    {
      char *tmpfile = format ("%s.saved", file);
      assert (link (file, tmpfile) == 0);
      reset_cloud_file_list(&state->source_files, &state->source_count);
      reset_cloud_file_list(&state->include_files, &state->include_count);
      append_to_cloud_file_list (&state->source_files, &state->source_count,
				 tmpfile, NULL);
      free (tmpfile);
    }
}

/* Wipe the recorded include files.
   Presumably the source scan was a dead end.  */
void
cloud_hook_reset_includes(void)
{
  reset_cloud_file_list(&state->include_files, &state->include_count);
}

void
cloud_hook_direct_mode_autodisabled (const char *reason)
{
  if (cloud_offline_mode())
    return;

  state->direct_mode_autodisabled = reason;
}

void
cloud_hook_record_result_type (enum result_type result_type)
{
  if (cloud_offline_mode())
    return;

  state->result_type = result_type;
}

/* Initialize networking etc. Returns true on success, or false (zero) if
   unsuccessful (and enter offline mode).  */
int
cloud_initialize(void)
{
  int fd;

  if (cloud_offline_mode())
    return 0;

  state = mmap (NULL, sizeof(struct state), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (state == MAP_FAILED)
    fatal ("Could not mmap memory: %s", strerror(errno));
  *state = initial_state;
  state->free = state->shared_heap;

  if (strcmp (conf->cloud_user_key, "") == 0)
    {
      cc_log("No Cloud Sourcery Key is configured; reverting to offline mode.");
      conf->cloud_mode = "offline";
      fprintf (stderr,
               "cs: error: No license key found.\n"
               "Need a key? Visit https://www.cloudsourcery.com/cs_keys\n"
               "Already have a key? Enter your key in a file named \".cs\" in your $HOME folder:\n"
               "  cloud_key = <key>\n"
               "You can also set the key as an environment variable: CS_KEY=<key value>\n"
               "Continuing in offline mode ...\n");
      return 0;
    }

  /* Create the header strings we'll pass to the server later.  */
  user_key_header = format ("X-USER-KEY: %s", conf->cloud_user_key);

  /* Add a random `ID' number to the headers.
     This is so the server can match GET and POST requests from this client.  */
  fd = open ("/dev/urandom", O_RDONLY);
  if (fd != -1)
    {
      int client_id;
      if (read (fd, &client_id, sizeof(client_id)) == 4)
	client_id_header = format ("X-CLIENT-SESSION-ID: %u", client_id);
      close (fd);
    }

  exitfn_add_nullary (post_results_to_cloud);

  return 1;
}

/* Exit the program, successfully or otherwise.
   We can't implement this as an atexit function because we want to post
   the reason and status code to the cloud.  */
void
cloud_exit(const char *reason, int status)
{
  if (!cloud_offline_mode())
    {
      state->exit_reason = reason;
      state->exit_status = status;
    }

  /* NOTE: The exit handler includes post_results_to_cloud.  */

  exit(status);
}
