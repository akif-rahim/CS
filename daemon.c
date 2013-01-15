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

/* This file creates and maintains a daemon process that allows multiple
   cs instances to share a persistent SSL connection, and thereby eliminate
   the handshake overhead from cloud cache queries.

   The Internet communication in done using Libcurl.

   The inter-process communication is done via Unix Domain Sockets.  The
   socket file is located in the cache directory. There will be one daemon
   per cache directory. If the daemon is not used for a set period of time it
   will quietly close down.

   The daemon is started either manually, through "cs --daemon" (primarily
   for debug), or automatically whenever cs finds no socket file.

   The socket file is named '$CS_CACHE_DIR/daemon.<user>.<host>.<n>' where the
   '<n>' indicates the local communications protocol revision in use. By
   this means, different versions of cs with incompatible daemons can coexist
   in the same cache.

   The daemon is single-threaded, but can comminicate with multiple clients
   using non-blocking I/O and 'select'. The Internet communications use
   Libcurl's 'multi' interface to maintain a pool of connections. The size
   of the pool is scaled automatically according to the size of the local
   backlog. */

#include "ccache.h"
#include "daemon.h"
#include "conf.h"
extern struct conf *conf;

#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pwd.h>
#include <sys/utsname.h>

//#define DISABLE_DAEMON 1
//#define DEBUG 1

#ifndef DISABLE_DAEMON
#define DISABLE_DAEMON 0
#endif
#ifndef DEBUG
#define DEBUG 0
#endif

#define LOCAL_PROTOCOL_REVISION 1

struct local_connection_state
{
  int fd;
  unsigned int client_number;
  unsigned int job_number;

  char *url;
  struct curl_slist *header_list;
  struct curl_httppost *post;
  struct curl_httppost *last;
  struct mmap {
    void *addr;
    size_t length;
    struct mmap *next;
  } *mmaps;

  struct timeval request_time;

  struct internet_connection_state *iconn;
  struct server_response *response;

  enum {
    STATE_RECV_INIT,
    STATE_RECV_SIZE,
    STATE_RECV_URL,
    STATE_RECV_HEADER,
    STATE_RECV_FORM_NAME,
    STATE_RECV_FORM_DATA,
    STATE_RECV_ATTACHMENT_NAME,
    STATE_RECV_ATTACHMENT_FILE,
    STATE_RECV_ATTACHMENT_FILENAME,
    STATE_RECV_ATTACHMENT_COMPLETE,
    STATE_WAITING,
    STATE_INPROGRESS,
    STATE_SEND_INIT,
    STATE_SEND_DATA_HEADER,
    STATE_SEND_DATA_BODY,
    STATE_SEND_ATTACHMENT_HEADER,
    STATE_SEND_ATTACHMENT_FAILNAME,
    STATE_SEND_ATTACHMENT_TMPFILENAME,
    STATE_SEND_DR_DONE,
    STATE_RESET
  } dfa_state, dfa_next_state;
  char *current_data, *stashed_string[3], *send_buffer;
  size_t current_offset, current_size;
  union daemon_responses *dr;

  int response_begun;
  struct response_part *current_part;

  struct local_connection_state *next;
};

struct job_queue {
  struct local_connection_state *conn;
  struct job_queue *next;
};

struct internet_connection_state
{
  CURL *curl_handle;
  char curl_error_buffer[CURL_ERROR_SIZE];

  unsigned int connection_number;
  bool active;
  struct server_response *response;
  struct local_connection_state *lconn;

  struct timeval request_time;

  struct internet_connection_state *next;
};

static bool curl_initialized = false;

/* Unix Domain Socket global state.  */
enum {FD_READ, FD_WRITE};
fd_set open_local_fds[2];
static int master_socket = -1;
static char *master_socket_path = NULL;
int nfds = 0;
static struct local_connection_state *local = NULL, *last_local = NULL;
static struct job_queue *job_queue = NULL;
static struct job_queue *last_queued_get = NULL, *last_queued_post = NULL;
static int active_clients = 0;
static int waiting_jobs = 0;
static unsigned int client_counter = 0;
static unsigned int get_request_counter = 0;
static unsigned int post_request_counter = 0;
static double lowest_get_response_time = 0;
static double highest_get_response_time = 0;
static double lowest_post_response_time = 0;
static double highest_post_response_time = 0;
static double average_get_response_time = 0;
static double average_post_response_time = 0;

/* Internet (cURL) global state.  */
CURLM *multi_handle = NULL;
static struct internet_connection_state *internet = NULL;
static int active_internet_connection_count = 0;
static int internet_pool_count = 0;
static unsigned int internet_request_counter = 0;
static double lowest_internet_get_response_time = 0;
static double highest_internet_get_response_time = 0;
static double lowest_internet_post_response_time = 0;
static double highest_internet_post_response_time = 0;
static double average_internet_get_response_time = 0;
static double average_internet_post_response_time = 0;

static struct internet_connection_state *init_new_easy_handle(void);
static int set_url(struct local_connection_state *conn, char *url);
static int add_header(struct local_connection_state *conn,
                      const char *header);
static int add_form_data(struct local_connection_state *conn,
                         const char *name, const char *data);
static int add_form_attachment(struct local_connection_state *conn,
                               const char *name, const char *shared_name,
                               size_t size, const char *filename);
static int get_response(struct local_connection_state *conn,
                        union daemon_responses **dr_ptr);
static void setup_internet_request (struct internet_connection_state *iconn,
                                    struct local_connection_state *lconn);

#define FREE(PTR) do {free(PTR); PTR = NULL;} while (0)

/* Call getcwd in a standards compatible way such that it doesn't fail.
   The caller should free the returned pointer.  */
static char *
get_working_directory (void)
{
  char *cwd = NULL, *result = NULL;
  int size = 0;
  do
    {
      size += 100;
      cwd = x_realloc (cwd, size);
      result = getcwd (cwd, size);
    }
  while (!result && errno == ERANGE);
  assert (result);

  return cwd;
}

/* Return the name of the local machine.
   The caller should free the returned pointer.  */
static char *
get_host_name (void)
{
  struct utsname uname_info;
  uname (&uname_info);
  return x_strdup (uname_info.nodename);
}

/* -----------------------------------------------------------------------*/
/* These functions are part of the client. They launch the daemon, connect
   to an already running deamon, and handle the client-side of the local
   communications.

   If DISABLE_DAEMON is set as build time, they do the Internet
   communications directly.  */

/* Launch the daemon process. Return 1 if the fork was successful, and
   zero otherwise. */
int
launch_daemon (void)
{
  int pid;

  if (DISABLE_DAEMON)
    {
      cc_log ("Daemon disabled at build time");

      /* Launching a non-existent daemon is always successful.  */
      return 1;
    }

  /* Fork the daemon process.  */
  pid = fork ();
  if (pid == 0)
    {
      /* In daemon. */
      execlp (cs_argv0, cs_argv0, "--daemon", (char *)NULL);

      /* Shouldn't get here.  */
      cc_log ("Failed to exec daemon: %s", strerror (errno));
      return 0;
    }
  else if (pid == -1)
    cc_log ("Error starting daemon: %s", strerror (errno));
  else
    /* In parent. */
    return 1;

  return 0;
}

daemon_handle
connect_to_daemon (void)
{
  if (DISABLE_DAEMON)
    {
      local = x_calloc (1, sizeof (*local));
      return init_new_easy_handle () != NULL;
    }
  struct timeval starttime, endtime, timediff;
  gettimeofday(&starttime, NULL);

  int newfd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (newfd == -1)
    {
      cc_log ("Error: couldn't open unix domain socket: %s", strerror (errno));
      return 0;
    }

  // Build the socket name
  struct sockaddr_un addr = {AF_UNIX, ""};
  char *host_name = get_host_name ();
  char *socket_path = format ("daemon.%d.%s.%d", geteuid (), host_name,
                              LOCAL_PROTOCOL_REVISION);
  strcpy (addr.sun_path, socket_path);
  free (socket_path);
  free (host_name);

  char *cwd = get_working_directory ();
  if (chdir (conf->cache_dir) == -1
      || connect (newfd, &addr, sizeof(addr)) == -1)
    {
      cc_log ("Couldn't connect to %s/%s: %s", conf->cache_dir, addr.sun_path,
              strerror (errno));

      /* Launch a new daemon.  */
      cc_log ("Attempting to launch a fresh daemon");
      unlink (addr.sun_path);
      launch_daemon ();

      /* Connect to the new daemon.
         Try repeatedly for two seconds at 0.01s intervals.  */
      int connected = -1;
      for (int t = 0; t < 200; t++)
	{
	  connected = connect (newfd, &addr, sizeof(addr));
	  if (connected != -1)
	    break;
	  else
	    usleep (10000);  /* 0.01 seconds */
	}
      if (connected == -1)
	{
	  cc_log ("Could not connect to daemon after 2 seconds: %s",
	          strerror (errno));
	  close (newfd);
	  if (chdir (cwd) == -1)
	    { /* Silence warning */ }
	  free (cwd);
	  return 0;
	}
    }
  if (chdir (cwd) == -1)
    { /* Silence warning */ }
  free (cwd);

  gettimeofday(&endtime, NULL);
  timersub(&endtime, &starttime, &timediff);
  cc_log ("daemon connect time: %ld.%06ld", timediff.tv_sec, timediff.tv_usec);

  /* Success!  */
  return newfd;
}

void
close_daemon (daemon_handle dh)
{
  close (dh);
}

static int
send_all (int fd, const char *data, ssize_t length)
{
  int sent = 0;
  while (sent < length)
    {
      int n = send (fd, data + sent, length - sent, 0);
      if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
	{
	  cc_log ("error: send failed: %s", strerror (errno));
	  return 0;
	}
      sent += n;
    }
  return 1;
}

static int
recv_all (int fd, char *data, ssize_t length)
{
  int received = 0;
  while (received < length)
    {
      int n = recv (fd, data + received, length - received, 0);
      if (n == 0)
	{
	  cc_log ("error: recv failed: daemon disconnected");
	  return 0;
	}
      else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK
	       && errno != EINTR)
	{
	  cc_log ("error: recv failed: %s", strerror (errno));
	  return 0;
	}
      received += n;
    }
  return 1;
}

int
set_daemon_url (daemon_handle dh, char *url)
{
  if (DISABLE_DAEMON)
    return set_url (local, x_strdup (url));

  if (dh == -1)
    return 0;

  int url_length = strlen (url);
  char buffer[5];
  buffer[0] = 'U';
  buffer[1] = url_length & 0xFF;
  buffer[2] = (url_length & 0xFF00) >> 8;
  buffer[3] = (url_length & 0xFF0000) >> 16;
  buffer[4] = (url_length & 0xFF000000) >> 24;

  if (DEBUG)
    cc_log ("client sending 'U'");
  return send_all (dh, buffer, 5)
      && send_all (dh, url, url_length);
}

int
add_daemon_header (daemon_handle dh, const char *header)
{
  if (DISABLE_DAEMON)
    return add_header (local, header);

  if (dh == -1)
    return 0;

  int header_length = strlen (header);
  char buffer[5];
  buffer[0] = 'H';
  buffer[1] = header_length & 0xFF;
  buffer[2] = (header_length & 0xFF00) >> 8;
  buffer[3] = (header_length & 0xFF0000) >> 16;
  buffer[4] = (header_length & 0xFF000000) >> 24;

  if (DEBUG)
    cc_log ("client sending 'H' (%s)", header);
  return send_all (dh, buffer, 5)
      && send_all (dh, header, header_length);
}

int
add_daemon_form_data (daemon_handle dh, const char *name, const char *data)
{
  if (DISABLE_DAEMON)
    return add_form_data(local, name, data);

  if (dh == -1)
    return 0;

  int name_length = strlen (name);
  char buffer1[5];
  buffer1[0] = 'F';
  buffer1[1] = name_length & 0xFF;
  buffer1[2] = (name_length & 0xFF00) >> 8;
  buffer1[3] = (name_length & 0xFF0000) >> 16;
  buffer1[4] = (name_length & 0xFF000000) >> 24;

  int data_length = strlen (data);
  char buffer2[4];
  buffer2[0] = data_length & 0xFF;
  buffer2[1] = (data_length & 0xFF00) >> 8;
  buffer2[2] = (data_length & 0xFF0000) >> 16;
  buffer2[3] = (data_length & 0xFF000000) >> 24;

  if (DEBUG)
    cc_log ("client sending 'F'");
  return send_all (dh, buffer1, 5)
      && send_all (dh, name, name_length)
      && send_all (dh, buffer2, 4)
      && send_all (dh, data, data_length);
}

int
add_daemon_form_attachment (daemon_handle dh, const char *name,
                            struct stashed_file *sf, const char *filename)
{
  size_t map_size = sizeof (*sf) + sf->size;
  if (DISABLE_DAEMON)
    return add_form_attachment(local, name, sf->shm_name, map_size, filename);

  if (dh == -1)
    return 0;

  int name_length = strlen (name);
  char buffer1[5];
  buffer1[0] = 'A';
  buffer1[1] = name_length & 0xFF;
  buffer1[2] = (name_length & 0xFF00) >> 8;
  buffer1[3] = (name_length & 0xFF0000) >> 16;
  buffer1[4] = (name_length & 0xFF000000) >> 24;

  int share_length = strlen (sf->shm_name);
  char buffer2[4];
  buffer2[0] = share_length & 0xFF;
  buffer2[1] = (share_length & 0xFF00) >> 8;
  buffer2[2] = (share_length & 0xFF0000) >> 16;
  buffer2[3] = (share_length & 0xFF000000) >> 24;

  int filename_length = strlen (filename);
  char buffer3[4];
  buffer3[0] = filename_length & 0xFF;
  buffer3[1] = (filename_length & 0xFF00) >> 8;
  buffer3[2] = (filename_length & 0xFF0000) >> 16;
  buffer3[3] = (filename_length & 0xFF000000) >> 24;

  char buffer4[4];
  buffer4[0] = map_size & 0xFF;
  buffer4[1] = (map_size & 0xFF00) >> 8;
  buffer4[2] = (map_size & 0xFF0000) >> 16;
  buffer4[3] = (map_size & 0xFF000000) >> 24;

  if (DEBUG)
    cc_log ("client sending 'A'");
  return send_all (dh, buffer1, 5)
      && send_all (dh, name, name_length)
      && send_all (dh, buffer2, 4)
      && send_all (dh, sf->shm_name, share_length)
      && send_all (dh, buffer3, 4)
      && send_all (dh, filename, filename_length)
      && send_all (dh, buffer4, 4);
}

int
request_daemon_response (daemon_handle dh)
{
  if (DISABLE_DAEMON)
    {
      setup_internet_request (internet, local);
      curl_easy_perform (internet->curl_handle);
      local->response = internet->response;
      internet->response = NULL;
      internet->lconn = NULL;
      internet->active = false;
      return 1;
    }

  if (dh == -1)
    return 0;

  if (DEBUG)
    cc_log ("client sending 'R'");
  /* Tell the daemon to sent the request and return the response.  */
  return send_all (dh, "R", 1);
}

enum daemon_response_codes
get_daemon_response (daemon_handle dh, union daemon_responses **dr_ptr)
{
  if (DISABLE_DAEMON)
    return get_response(local, dr_ptr);

  if (dh == -1)
    return D_REQUEST_FAILED;

  while (1)
    {
      char buffer[12];
      size_t headersize, datasize, filenamesize, tmp_filenamesize;
      char *headers, *data, *filename, *tmp_filename;

      if (!recv_all (dh, buffer, 1))
	return D_RESPONSE_INCOMPLETE;

      if (DEBUG)
        cc_log ("client received '%c'", buffer[0]);
      switch (buffer[0])
        {
	case 'F':
	  return D_REQUEST_FAILED;

	case 'E':
	  return D_RESPONSE_INCOMPLETE;

	case 'C':
	  return D_RESPONSE_COMPLETE;

	case 'R':
	  if (!recv_all (dh, buffer, 2))
	    return D_RESPONSE_INCOMPLETE;

	  *dr_ptr = x_malloc (sizeof (**dr_ptr));
	  (*dr_ptr)->http_result_code = ((buffer[0] & 0xFF)
					 | ((buffer[1] & 0xFF) << 8));
	  return D_HTTP_RESULT_CODE;

	case 'D':
	  if (!recv_all (dh, buffer, 8))
	    return D_RESPONSE_INCOMPLETE;

	  headersize = ((buffer[0] & 0xFF)
	                | ((buffer[1] & 0xFF) << 8)
	                | ((buffer[2] & 0xFF) << 16)
	                | ((buffer[3] & 0xFF) << 24));
	  datasize = ((buffer[4] & 0xFF)
	              | ((buffer[5] & 0xFF) << 8)
	              | ((buffer[6] & 0xFF) << 16)
	              | ((buffer[7] & 0xFF) << 24));

	  headers = x_malloc (headersize + 1);
	  headers[headersize] = '\0';
	  if (!recv_all (dh, headers, headersize))
	    {
	      free (headers);
	      return D_RESPONSE_INCOMPLETE;
	    }

	  data = x_malloc (datasize + 1);
	  data[datasize] = '\0';
	  if (!recv_all (dh, data, datasize))
	    {
	      free (headers);
	      free (data);
	      return D_RESPONSE_INCOMPLETE;
	    }

	  *dr_ptr = x_malloc (sizeof (**dr_ptr));
	  (*dr_ptr)->body.headers = headers;
	  (*dr_ptr)->body.headersize = headersize;
	  (*dr_ptr)->body.data = data;
	  (*dr_ptr)->body.datasize = datasize;
	  return D_BODY;

	case 'A':
	  if (!recv_all (dh, buffer, 12))
	    return D_RESPONSE_INCOMPLETE;

	  headersize = ((buffer[0] & 0xFF)
	                | ((buffer[1] & 0xFF) << 8)
	                | ((buffer[2] & 0xFF) << 16)
	                | ((buffer[3] & 0xFF) << 24));
	  filenamesize = ((buffer[4] & 0xFF)
	                  | ((buffer[5] & 0xFF) << 8)
	                  | ((buffer[6] & 0xFF) << 16)
	                  | ((buffer[7] & 0xFF) << 24));
	  tmp_filenamesize = ((buffer[8] & 0xFF)
	   	              | ((buffer[9] & 0xFF) << 8)
	   	              | ((buffer[10] & 0xFF) << 16)
	   	              | ((buffer[11] & 0xFF) << 24));

	  headers = x_malloc (headersize + 1);
	  headers[headersize] = '\0';
	  if (!recv_all (dh, headers, headersize))
	    {
	      free (headers);
	      return D_RESPONSE_INCOMPLETE;
	    }

	  filename = x_malloc (filenamesize + 1);
	  filename[filenamesize] = '\0';
	  if (!recv_all (dh, filename, filenamesize))
	    {
	      free (headers);
	      free (filename);
	      return D_RESPONSE_INCOMPLETE;
	    }

	  tmp_filename = x_malloc (tmp_filenamesize + 1);
	  tmp_filename[tmp_filenamesize] = '\0';
	  if (!recv_all (dh, tmp_filename, tmp_filenamesize))
	    {
	      free (headers);
	      free (filename);
	      free (tmp_filename);
	      return D_RESPONSE_INCOMPLETE;
	    }

	  *dr_ptr = x_malloc (sizeof (**dr_ptr));
	  (*dr_ptr)->attachment.headers = headers;
	  (*dr_ptr)->attachment.headersize = headersize;
	  (*dr_ptr)->attachment.filename = filename;
	  (*dr_ptr)->attachment.tmp_filename = tmp_filename;
	  return D_ATTACHMENT;
        }
    }
}

/* Consume unwanted remaining responses to prevent them being queued for later.
   Do NOT call this if D_RESPONSE_INCOMPLETE, D_RESPONSE_COMPLETE or
   D_REQUEST_FAILED has already been received.  */
void
flush_daemon_response (daemon_handle dh)
{
  while (1)
    {
      union daemon_responses *dr;
      switch (get_daemon_response(dh, &dr))
      {
	case D_REQUEST_FAILED:
	case D_RESPONSE_INCOMPLETE:
	case D_RESPONSE_COMPLETE:
	  return;
	case D_HTTP_RESULT_CODE:
	case D_BODY:
	case D_ATTACHMENT:
	  free (dr);
	  break;
      }
    }
}

/* -----------------------------------------------------------------------*/
/* These functions are only used in the daemon process
   (unless DISABLE_DAEMON is set at build time).  */

static size_t receive_cloud_response_headers (char *data, size_t blocksize,
                                              size_t nblocks, void *userdata);
static size_t receive_cloud_response (char *netdata, size_t blocksize,
                                      size_t nblocks, void *userdata);

/* Warning, MIN evaluates X and Y twice.
   This is for convenience only.  */
#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))

struct server_response
{
  int code;
  bool complete;
  char *type;
  size_t size;
  char *boundary;
  struct response_part
  {
    char *headers;
    size_t headersize;
    char *type;
    char *data;
    size_t datasize;
    char *filename, *tmp_filename;
    FILE *fd;
    struct response_part *next;
  } *parts;
  struct response_part *last;
  char *data_stash;
  size_t data_stash_size;
};

/* Destruct server response data.  */
static void
free_server_response (struct server_response *response)
{
  struct response_part *part, *nextpart;

  if (!response)
    return;

  free (response->type);
  free (response->boundary);

  for (part = response->parts; part; part = nextpart)
    {
      free (part->headers);
      free (part->data);
      free (part->type);
      free (part->filename);
      free (part->tmp_filename);
      nextpart = part->next;
      free (part);
    }

  free (response);
}

/* Wrapper for curl_easy_setopt.
   Returns zero on failure, and writes the reason to the log.  */
static int
x_curl_easy_setopt(struct internet_connection_state *conn,
                   CURLoption opt, void *val)
{
  CURLcode code = curl_easy_setopt (conn->curl_handle, opt, val);

  if (code != 0)
    {
      cc_log("<%u> curl_easy_setopt failed with code %d (%s)",
             conn->connection_number, code, conn->curl_error_buffer);
      conn->curl_error_buffer[0] = '\0';
      return 0;
    }

  return 1;
}

/* Wrapper for curl_formadd.
   This has to be a macro because libcurl does not provide va_list variants.  */
#define x_curl_formadd(CONN, ...) \
  do { \
    CURLFORMcode code = curl_formadd(&CONN->post, &CONN->last, ##__VA_ARGS__); \
    if (code != 0) \
      { \
	cc_log("[%u:%u] curl_formadd failed with code %d", \
	       conn->client_number, conn->job_number, code); \
      } \
  } while (0)

/* Create a new curl connection with the default settings, and insert it
   into the global connection pool.  */
static struct internet_connection_state *
init_new_easy_handle (void)
{
  char *str;
  struct internet_connection_state *new_connection =
	  calloc (1, sizeof (*new_connection));

  if (!curl_initialized && curl_global_init (CURL_GLOBAL_ALL) != 0)
    {
      cc_log("ERROR: libcurl initialization failed");
      free (new_connection);
      return NULL;
    }
  else
    curl_initialized = true;

  new_connection->curl_handle = curl_easy_init();
  if (new_connection->curl_handle == NULL)
    {
      cc_log("ERROR: libcurl could not create a new easy handle.");
      free (new_connection);
      return NULL;
    }

  if (!x_curl_easy_setopt(new_connection, CURLOPT_ERRORBUFFER,
                          new_connection->curl_error_buffer))
    cc_log("could not configure libcurl error buffer; any errors will not be logged.");

  if (0)
    x_curl_easy_setopt(new_connection, CURLOPT_VERBOSE, (void*)(long)1);

  x_curl_easy_setopt(new_connection, CURLOPT_FOLLOWLOCATION, (void*)(long)1);

  /* Set up callbacks.  */
  x_curl_easy_setopt(new_connection, CURLOPT_WRITEFUNCTION,
		     (void*) receive_cloud_response);
  x_curl_easy_setopt(new_connection, CURLOPT_HEADERFUNCTION,
		     (void*) receive_cloud_response_headers);

  /* Set timeout to 10 minutes. This is primarily to prevent the background
       POST task from sitting around forever in the case of a network or server
       problem.  It needs to be high enough not to break largish transfers over
       slow connections.  */
  x_curl_easy_setopt(new_connection, CURLOPT_TIMEOUT, (void*)(long)600);

  /* Add an x-cs-user-agent header. Libcurl will set the regular user-agent.  */
  str = format ("cs/%s (" __DATE__ " " __TIME__ ") %s", CS_VERSION, curl_version());
  x_curl_easy_setopt(new_connection, CURLOPT_USERAGENT, str);
  free (str);

  /* Add the new connection to the global connection pool.
     BEWARE: this list needs to be signal-safe.  */
  new_connection->next = internet;
  internet = new_connection;

  new_connection->connection_number = internet_pool_count++;
  cc_log ("<%u> Created new internet socket",
          new_connection->connection_number);

  return new_connection;
}

static void
reset_response (struct local_connection_state *conn)
{
  if (conn->response)
    {
      free_server_response(conn->response);
      conn->response = NULL;
    }
}

static int
set_url (struct local_connection_state *conn, char *url)
{
  reset_response(conn);
  if (conn->url)
    free (conn->url);
  conn->url = url;
  return 1;
}

static int
add_header (struct local_connection_state *conn, const char *header)
{
  reset_response(conn);
  if (DEBUG)
    cc_log ("[%u:%u] New header: %s", conn->client_number, conn->job_number,
            header);
  conn->header_list = curl_slist_append (conn->header_list, header);
  return 1;
}

static int
add_form_data (struct local_connection_state *conn,
               const char *name, const char *data)
{
  reset_response(conn);
  x_curl_formadd (conn,
                  CURLFORM_COPYNAME, name,
                  CURLFORM_COPYCONTENTS, data,
                  CURLFORM_END);
  return 1;
}

static int
add_form_attachment (struct local_connection_state *conn, const char *name,
                     const char *shared_name, size_t size, const char *filename)
{
  // Connect to an existing shared mapping.
  int shmd = shm_open (shared_name, 0, 0400);
  struct stashed_file *sf = mmap (NULL, size, PROT_READ, MAP_SHARED, shmd, 0);
  close (shmd);
  if (sf == MAP_FAILED || strcmp(sf->shm_name, shared_name) != 0)
    return 0;

  // Record the mapping for later cleanup
  struct mmap *map = x_malloc (sizeof (*map));
  map->addr = sf;
  map->length = size;
  map->next = conn->mmaps;
  conn->mmaps = map;

  // Add the attachment to the cURL form
  reset_response(conn);
  x_curl_formadd(conn,
                 CURLFORM_COPYNAME, name,
                 CURLFORM_BUFFER, filename,
                 CURLFORM_BUFFERPTR, sf->data,
                 CURLFORM_BUFFERLENGTH, sf->size,
                 CURLFORM_END);
  if (DEBUG)
    cc_log ("[%u:%u] Added attachment: [%s] %s", conn->client_number,
            conn->job_number, name, filename);
  return 1;
}

static void
cleanup_form (struct local_connection_state *conn)
{
  if (conn->post)
    curl_formfree (conn->post);
  conn->post = conn->last = NULL;

  while (conn->mmaps)
    {
      struct mmap *map = conn->mmaps;
      conn->mmaps = map->next;
      munmap (map->addr, map->length);
      free (map);
    }
}

/* This function returns a single aspect of the response message each time
   it is called.
   The data is placed in DR_PTR, and the caller should keep calling until
   D_REQUEST_FAILED, D_RESPONSE_INCOMPLETE, or D_RESPONSE_COMPLETE, and no
   further.  */
static int
get_response (struct local_connection_state *conn,
              union daemon_responses **dr_ptr)
{
  union daemon_responses *dr;

  if (!conn->response)
    return D_REQUEST_FAILED;

  /* We return the HTTP code first, and then iterate through the
     message parts on subsequent calls.  */
  if (!conn->response_begun)
    {
      conn->current_part = conn->response->parts;

      /* There's no point in returning half a message, so fail early. */
      if (!conn->response->complete)
	{
	  free_server_response (conn->response);
	  conn->response = NULL;
	  return D_RESPONSE_INCOMPLETE;
	}

      /* Return the HTTP code before anything else.  */
      dr = x_malloc (sizeof (*dr));
      dr->http_result_code = conn->response->code;
      *dr_ptr = dr;
      conn->response_begun = true;
      return D_HTTP_RESULT_CODE;
    }

  enum daemon_response_codes retval = 0;

  /* Return the next part in the list, if any.  */
  dr = x_malloc (sizeof (*dr));
  while (conn->current_part && !retval)
    {
      if (conn->current_part->data)
	{
	  dr->body.headers = conn->current_part->headers;
	  dr->body.headersize = conn->current_part->headersize;
	  dr->body.data = conn->current_part->data;
	  dr->body.datasize = conn->current_part->datasize;
	  retval = D_BODY;
	}
      else if (conn->current_part->filename)
	{
	  dr->attachment.headers = conn->current_part->headers;
	  dr->attachment.headersize = conn->current_part->headersize;
	  dr->attachment.filename = conn->current_part->filename;
	  dr->attachment.tmp_filename = conn->current_part->tmp_filename;
	  retval = D_ATTACHMENT;
	}

      conn->current_part = conn->current_part->next;
    }

  /* If we didn't find another part, finalize the message, and clean up.  */
  if (!retval && !conn->current_part)
    {
      free (dr);
      free_server_response (conn->response);
      conn->response = NULL;
      conn->response_begun = false;
      return D_RESPONSE_COMPLETE;
    }

  *dr_ptr = dr;
  return retval;
}

/* The call-back function for CURLOPT_HEADERFUNCTION.  */
static size_t
receive_cloud_response_headers (char *data, size_t blocksize,
                                size_t nblocks, void *userdata)
{
  struct server_response *response = (struct server_response *)userdata;
  size_t size = blocksize * nblocks;

  if (size < 13)
    /* The data is too short to be anything interesting,
       and we mustn't read past the end.  */
    ;
  else if (sscanf (data, "HTTP/1.1 %d", &response->code) == 1)
    ;
  else if (strncmp (data, "Content-Type: ",
                    MIN(size, strlen ("Content-Type: "))) == 0)
    {
      /* We support two content type formats:
         1. A plain "<mime-type>", or
         2. "multipart/mixed; boundary=<str>;"  */
      size_t len = size;
      char *end;
      data += strlen ("Content-Type: ");
      len -= strlen ("Content-Type: ");
      end = memchr (data, ';', len);
      if (!end)
	end = data + len;
      free (response->type);
      response->type = x_strndup (data, end - data);
      strtok (response->type, "\r\n");
      if (strncmp (data, "multipart/mixed",
                   MIN (len, strlen ("multipart/mixed"))) == 0)
	{
	  /* We have a multipart message. Extract the boundary string.  */
	  len -= (end - data) + 1;
	  data = end + 1;
	  if (strncmp (data, " boundary=\"",
	               MIN (len, strlen (" boundary=\""))) == 0)
	    {
	      data += strlen (" boundary=\"");
	      len -= strlen (" boundary=\"");
	      end = memchr (data, '"', len);
	      if (end)
		{
		  size_t b_len =  (end - data) + 4;
		  free (response->boundary);
		  response->boundary = x_malloc (b_len + 1);
		  response->boundary[0] = '\r';
		  response->boundary[1] = '\n';
		  response->boundary[2] = '-';
		  response->boundary[3] = '-';
		  strncpy (response->boundary + 4, data, end - data);
		  response->boundary[b_len] = '\0';
		}
	    }
	}
    }
  else if (sscanf (data, "Content-Length: %zu", &response->size) == 1)
    {
    }

  return size;
}

/* The call-back function for CURL_WRITE_FUNCTION.
   This should be called *again* after curl_easy_perform* has returned
   with DATA=NULL, BLOCKSIZE=0, NBLOCKS=0 to flush the buffer.  */
static size_t
receive_cloud_response (char *netdata, size_t blocksize, size_t nblocks,
                        void *userdata)
{
  char *data = netdata;
  struct server_response *response = (struct server_response *)userdata;
  size_t orig_size = blocksize * nblocks;
  size_t size = orig_size;
  struct response_part *part = response->last;
  size_t boundary_len = response->boundary ? strlen (response->boundary) : 0;
  char * must_free_data = NULL;

  /* If the end of the data has already been seen,
     we just ignore any more data.  */
  if (response->complete)
    return orig_size;

  /* This buffer is used to stash the tail of the data if the end boundary is
     *not* found.  This means we can be sure we don't miss it if it gets
     cut in half by chance.  */
  if (response->data_stash)
    {
      char *new_data = x_malloc (size + response->data_stash_size);
      memcpy (new_data, response->data_stash, response->data_stash_size);
      if (data)
	memcpy (new_data + response->data_stash_size, data, size);
      data = new_data;
      size += response->data_stash_size;
      free (response->data_stash);
      response->data_stash = NULL;
      response->data_stash_size = 0;
      must_free_data = data;
    }

  /* if we were called with no data, and there was no stash to clean up,
     then there's nothing to do.  */
  if (!data)
    return 0;

  if (!part)
    {
      /* Even non-multipart messages have one part.  */
      part = x_calloc (1, sizeof(struct response_part));
      response->parts = response->last = part;
    }

  while (size > boundary_len
	 || (netdata == NULL && size > 0))
    {
      char *begin = data;
      char *end;
      bool end_of_part_found = false;

      /* Find how much of the data is in the current part.  */
      if (response->boundary)
	{
	  end = memmem (data, size, response->boundary, boundary_len);
	  if (!end)
	    {
	      if (netdata == NULL)
		end = data + size;
	      else
		/* Ensure that the boundary wasn't chopped in two.  */
		end = data + size - boundary_len;
	    }
	  else
	    end_of_part_found = true;
	}
      else
	end = data + size;

      /* Read the part headers, if we haven't already.
         Single-part messages and part-zero of multipart messages
         do not have headers.  */
      if (part != response->parts && !part->fd && !part->data)
	{
	  bool header_complete = false;

	  /* Keep reading lines until there are two EOLNs in a row.  */
	  while (data < end)
	    {
	      char *line = data;
	      int line_length;
	      char *eoln = memmem (line, end - line, "\r\n", 2);

	      if (eoln)
		data = eoln + 2;
	      else
		break;

	      line_length = data - line;

	      /* If the line consists only of CRLF then we've read the whole
	         header already.  */
	      if (line_length == 2)
		{
		  header_complete = true;
		  break;
		}
	      else
		{
		  part->headers = x_realloc (part->headers,
					     part->headersize + line_length
					     + 1 /* nul-terminator */);
		  memcpy (part->headers + part->headersize, line, line_length);
		  part->headersize += line_length;
		  part->headers[part->headersize] = '\0';
		}
	    }

	  if (header_complete && part->headers)
	    {
	      /* We've read all the headers into a string.
	         Now parse them.  */
	      char *headers = part->headers;
	      while (1)
		{
		  char *linefeed = strchrnul (headers, '\n');
		  char *lineend = linefeed > headers && linefeed[-1] == '\r'
				  ? linefeed - 1: linefeed;
		  if (str_startswith (headers, "Content-Type: "))
		    {
		      /* Extract the MIME type.  */
		      headers += strlen ("Content-Type: ");
		      free (part->type);
		      part->type = x_strndup (headers, lineend - headers);
		    }
		  else if (str_startswith (headers, "Content-Disposition: "))
		    {
		      /* Extract the attachment filename, if any.  */
		      headers += strlen ("Content-Disposition: ");
		      if (str_startswith (headers, "attachment; filename="))
			{
			  char *nameend;
			  const char *basename, *tmp_file;

			  headers += strlen ("attachment; filename=");
			  nameend = strchr (headers, ';');
			  if (!nameend || nameend > lineend)
			    nameend = lineend;

			  free(part->filename);
			  part->filename = x_strndup (headers,
			                              nameend - headers);

			  /* We write attachments directly to file.  */
			  basename = strrchr (part->filename, '/');
			  if (!basename)
			    basename = part->filename;

			  tmp_file = tmp_string ();

			  free (part->tmp_filename);
			  part->tmp_filename = format ("%s/download.%s.%s",
			                               temp_dir(),
			                               basename, tmp_file);
			  part->fd = fopen (part->tmp_filename, "wb");
			  if (!part->fd)
			    fatal ("Could not open file %s for writing.",
			           part->tmp_filename);
			}
		    }
		  if (linefeed[0] == '\0')
		    break;
		  else
		    headers = linefeed + 1;
		}
	    }
	  else if (header_complete)
	    /* Zero-length headers are legal.  We just record the data.  */
	    part->data = x_strdup ("");
	  else
	    {
	      /* Incomplete headers.  Wait for more data.  */
	      size -= data - begin;
	      break;
	    }
	}

      /* If we get here, and we haven't run out of data, then the part headers
         have been read, and a file descriptor will have been opened if
         the part contains an attachment.  */

      if (data != end)
	{
	  /* Read the rest of the part.  */
	  if (part->fd)
	    {
	      /* Write the data to file.  */
	      if (!fwrite (data, end-data, 1, part->fd))
		fatal ("Error fwrite failed!");
	    }
	  else
	    {
	      /* Store the data in a memory buffer.  */
	      part->data = x_realloc (part->data,
				      part->datasize + (end - data)
				      + 1 /* nul-terminator */);
	      memcpy (part->data + part->datasize, data, end - data);
	      part->datasize += end - data;
	      part->data[part->datasize] = '\0';
	    }
	}

      size -= end - begin;
      data = end;

      /* If we get here, and we haven't run out of data, then there are
         more parts we can continue reading.  */

      if (end_of_part_found)
	{
	  /* The rest of the boundary line is ignored.  If we don't have
	     all of it then we'll need to stash the data and read more.  */
	  char *next_line = memmem (data+boundary_len, size-boundary_len,
	                            "\r\n", 2);
	  if (!next_line)
	    {
	      if (!netdata)
		next_line = data + size;
	      else
		/* No line terminator.  Wait for more data.  */
	        break;
	    }
	  else
	    /* Skip to next line.  */
	    next_line += 2;

	  /* Close the completed part's file, if any.  */
	  if (part->fd)
	    {
	      fclose (part->fd);
	      part->fd = NULL;
	    }

	  if (next_line > data + boundary_len
	      && str_startswith (data + boundary_len, "--"))
	    {
	      /* We have a final boundary line, ending with '--'.
	         There are no more parts. Any more data is junk.  */
	      response->complete = true;
	      break;
	    }
	  else
	    {
	      /* Begin reading a new part.  */
	      part->next = x_calloc (1, sizeof(struct response_part));
	      response->last = part->next;
	      part = part->next;

	      size -= next_line - data;
	      data = next_line;
	    }
	}
    }

  if (size > 0 && !response->complete)
    {
      /* Stash the remainder for next time.
         This only happens to avoid losing a boundary.  */
      response->data_stash = x_malloc (size);
      response->data_stash_size = size;
      memcpy (response->data_stash, data, size);
    }

  /* Multipart responses are completed by a final binary, which is checked
     above, but single-part responses are complete if they are the right
     length.  */
  if (!response->boundary && part->datasize == response->size)
    response->complete = true;

  if (must_free_data)
    free (must_free_data);
  return orig_size;
}

/* Add a new job into the queue. GET requests are inserted before POST
   requests.  */
static void
queue_new_job (struct local_connection_state *conn)
{
  struct job_queue *insert_after;
  struct job_queue *new_jq = x_malloc (sizeof (*new_jq));
  new_jq->conn = conn;

  if (conn->post)
    {
      insert_after = last_queued_post;
      last_queued_post = new_jq;
    }
  else
    {
      insert_after = last_queued_get;
      if (last_queued_post == last_queued_get)
	last_queued_post = new_jq;
      last_queued_get = new_jq;
    }

  if (!insert_after)
    {
      new_jq->next = job_queue;
      job_queue = new_jq;
    }
  else
    {
      new_jq->next = insert_after->next;
      insert_after->next = new_jq;
    }
}

/* Remove and return the first connection in the job_queue. */
static struct local_connection_state *
pop_queued_job ()
{
  struct local_connection_state *result = NULL;
  struct job_queue *job = job_queue;

  if (job)
    {
      result = job->conn;
      job_queue = job->next;

      cc_log ("pop: job = %p, jq = %p, lg = %p, lp = %p", job, job_queue, last_queued_get, last_queued_post);
      if (job == last_queued_get)
	last_queued_get = NULL;
      if (job == last_queued_post)
	last_queued_post = NULL;

      struct job_queue *tmp;
      int found = 0;
      for (tmp = job_queue; tmp; tmp = tmp->next)
	if (tmp == last_queued_get)
	  found = 1;
      if (last_queued_get)
	assert(found);
      if (!job_queue)
	assert (!last_queued_get && !last_queued_post);

      free (job);
    }

  return result;
}

/* Remove a job from the queue. Presumably the client has died.  */
static void
dequeue_job (struct local_connection_state *conn)
{
  struct job_queue **ptr, *prev = NULL;

  for (ptr = &job_queue; *ptr; ptr = &(*ptr)->next)
    if ((*ptr)->conn == conn)
      {
	if (last_queued_get == *ptr)
	  last_queued_get = prev;
	if (last_queued_post == *ptr)
	  last_queued_post = prev;
        *ptr = (*ptr)->next;
        break;
      }
    else
      prev = *ptr;
}

/* This function is only called when things get bad!
   It will cause the daemon to stop accepting new connections and, eventually,
   to exit, but not until any existing jobs are complete (via the usual
   inactivity timeout).
   It is assumed that any new clients will automatically launch a new daemon
   to replace this one.  */
static void
shutdown_master_socket ()
{
  FD_CLR (master_socket, &open_local_fds[FD_READ]);
  close (master_socket);
  master_socket = -1;
}

/* Accept all outstanding connection requests,
   and add them into the 'local' list. */
static void
accept_local_connections (void)
{
  int newfd;
  struct sockaddr newaddr;
  socklen_t newaddr_len;
  /* The master socket should be non-blocking, so we read until it
     returns nothing.  */
  while (1)
    {
      newaddr_len = sizeof (struct sockaddr);
      newfd = accept (master_socket, &newaddr, &newaddr_len);
      if (newfd == -1)
	{
	  if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
	    {
	      cc_log ("ERROR: daemon cannot read from master socket: %s",
	              strerror (errno));
	      shutdown_master_socket ();
	    }
	  break;
	}

      /* Configure the new connection.  */
      fcntl (newfd, F_SETFL, O_NONBLOCK);

      /* Add the new connection to the list.
         BEWARE: this list needs to be signal-safe.  */
      struct local_connection_state *new_lcs =
	  x_calloc (1, sizeof (*new_lcs));
      new_lcs->fd = newfd;
      new_lcs->client_number = client_counter++;
      if (local)
	{
	  last_local->next = new_lcs;
	  last_local = new_lcs;
	}
      else
	local = last_local = new_lcs;

      active_clients++;
      cc_log ("[%u] Accepted new client connection", new_lcs->client_number);

      /* Update the descriptor sets used by select.  */
      FD_SET (newfd, &open_local_fds[FD_READ]);
      if (newfd	>= nfds)
	nfds = newfd + 1;

      // Enough is enough!
      // If we're about to run out of file descriptors then we stop accepting
      // more connections and let another daemon take over.
      if (active_clients >= 900 || newfd >= 900)
	{
	  cc_log ("WARNING: The number of connected client is nearing the"
	          " limit of available file descriptors."
	          " This daemon will now stop accepting new connections"
	          " and another daemon must take over.");
	  shutdown_master_socket ();
	  break;
	}
    }
  cc_log ("Daemon now has %d client connections.", active_clients);
}

static void
close_local_connection (struct local_connection_state *conn)
{
  /* Find and remove CONN from the linked list.
     BEWARE: this list needs to be signal-safe.  */
  struct local_connection_state **ptr, *prev = NULL;
  for (ptr = &local; *ptr; ptr = &(*ptr)->next)
    if (*ptr == conn)
      {
	if (last_local == conn)
	  last_local = prev;
        *ptr = (*ptr)->next;
        break;
      }
    else
      prev = *ptr;

  cc_log ("[%u] Closing client connection", conn->client_number);

  /* Clean up the socket. */
  FD_CLR (conn->fd, &open_local_fds[FD_READ]);
  FD_CLR (conn->fd, &open_local_fds[FD_WRITE]);
  close (conn->fd);

  /* Clean up program state.  */
  if (conn->dfa_state == STATE_WAITING)
    {
      dequeue_job (conn);
      waiting_jobs--;
    }
  active_clients--;
  if (conn->iconn)
    conn->iconn->lconn = NULL;

  cc_log ("%d client connections remain", active_clients);

  /* Clean up memory.  */
  if (conn->header_list)
    curl_slist_free_all (conn->header_list);
  cleanup_form (conn);
  if (conn->response)
    free_server_response (conn->response);
  free (conn->url);
  free (conn->current_data);
  free (conn->stashed_string[0]);
  free (conn->stashed_string[1]);
  free (conn->send_buffer);
  free (conn->dr);
  free (conn);
}

/* Receive data from a local connection, and store it in conn->current_data.
   Returns 0 if the read blocked (select and call again), or 1 if the data
   is ready. The caller should reset conn->current_data to NULL before calling
   this function again, and should free the memory if no longer required.
   The INITIAL_SIZE is ignored if conn->current_data is non-NULL.
   The actual allocation size is INITIAL_SIZE+1, and the extra byte is zeroed.
   This call clobbers conn->current_offset and conn->current_size.  */
static int
recv_all_nonblock (struct local_connection_state *conn, size_t initial_size)
{
  if (!conn->current_data)
    {
      conn->current_data = x_malloc (initial_size+1);
      conn->current_data[initial_size] = '\0';
      conn->current_offset = 0;
      conn->current_size = initial_size;
    }

  size_t recv_size = recv (conn->fd, conn->current_data + conn->current_offset,
                           conn->current_size, O_NONBLOCK);
  switch (recv_size)
    {
    case 0:
      /* Unexpected client disconnect. */
      if (DEBUG)
	cc_log ("[%u] client disconnected???", conn->client_number);
      close_local_connection (conn);
      return 0;

    case -1:
      /* No data, but probably not a problem. */
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
	return 0;
      else if (DEBUG
	  || conn->dfa_state != STATE_RECV_INIT
	  || errno != ECONNRESET)
	cc_log ("[%u] Error: local connection error: %s", conn->client_number,
	        strerror (errno));
      return 0;

    default:
      /* Data received */
      conn->current_offset += recv_size;
      conn->current_size -= recv_size;
      break;
    }

  if (conn->current_size)
    return 0;
  else
    return 1;
}

/* Send data to a local connection, given data in INITIAL_DATA.
   Returns 0 if the send blocked (select and call again), or 1 if the send
   is complete.
   The INITIAL_DATA and INITIAL_SIZE is ignored if conn->send_buffer is
   non-NULL, so callers may test conn->send_buffer themselves, for efficiency.
   When the send is complete, conn->send_buffer will be NULL once more.
   This call clobbers conn->current_offset and conn->current_size.  */
static int
send_all_nonblock (struct local_connection_state *conn,
                   char *initial_data, size_t initial_size)
{
  if (!conn->send_buffer)
    {
      conn->send_buffer = initial_data;
      conn->current_offset = 0;
      conn->current_size = initial_size;
    }

  ssize_t sent_size = send (conn->fd, conn->send_buffer + conn->current_offset,
                            conn->current_size, O_NONBLOCK);
  if (sent_size == -1)
    {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
	{
	  cc_log ("[%u:%u] Error: local connection error: %s",
	          conn->client_number, conn->job_number, strerror (errno));
	  close_local_connection (conn);
	}
      return 0;
    }

  conn->current_offset += sent_size;
  conn->current_size -= sent_size;

  if (conn->current_size)
    return 0;
  else
    {
      conn->send_buffer = NULL;
      return 1;
    }
}

/* Read message data from a client, trigger the internet comms, and
   then return the response.  */
static void
do_local_comms (struct local_connection_state *conn)
{
  /* We use a state machine to permit resuming non-blocking I/O.  */
  while (1)
    {
      switch (conn->dfa_state)
	{
	case STATE_RECV_INIT:
	  /* Each packet starts with a single code character.  */
	  if (!recv_all_nonblock (conn, 1))
	    return;
	  if (DEBUG)
	    cc_log ("[%u:%u] received code 0x%02x: '%c'",
	            conn->client_number, conn->job_number,
	            conn->current_data[0], conn->current_data[0]);
	  conn->dfa_state = STATE_RECV_SIZE;
	  switch (conn->current_data[0])
	  {
	    case 'U':
	    conn->dfa_next_state = STATE_RECV_URL;
	    break;
	    case 'H':
	      conn->dfa_next_state = STATE_RECV_HEADER;
	      break;
	    case 'F':
	      conn->dfa_next_state = STATE_RECV_FORM_NAME;
	      break;
	    case 'A':
	      conn->dfa_next_state = STATE_RECV_ATTACHMENT_NAME;
	      break;
	    case 'R':
	      if (!conn->url)
		{
		  /* No URL was set, so we fail instantly.  */
		  cc_log ("[%u:%u] warning: client requested response with URL unset",
			  conn->client_number, conn->job_number);
		  conn->dfa_state = STATE_SEND_INIT;
		  break;
		}

	      cc_log ("[%u:%u] job ready", conn->client_number,
	              conn->job_number);

	      /* Add this to the queue of things to do ... */
	      conn->dfa_state = STATE_WAITING;
	      gettimeofday (&conn->request_time, NULL);
	      queue_new_job (conn);
	      waiting_jobs++;
	      break;
	    default:
	      cc_log ("[%u:%u] Daemon received unexpected code 0x%02x",
	              conn->client_number, conn->job_number,
	              conn->current_data[0]);
	      close_local_connection (conn);
	      return;
	  }
	  FREE (conn->current_data);
	  break;

	case STATE_RECV_SIZE:
	  /* Receive a 32-bit integer in little-endian.
	     This will be the data length of the next field.  */
	  if (!recv_all_nonblock (conn, 4))
	    return;
	  conn->current_size = ((conn->current_data[0] & 0xFF)
	                        | ((conn->current_data[1] & 0xFF) << 8)
	                        | ((conn->current_data[2] & 0xFF) << 16)
	                        | ((conn->current_data[3] & 0xFF) << 24));
	  FREE (conn->current_data);
	  conn->dfa_state = conn->dfa_next_state;
	  break;

	case STATE_RECV_URL:
	case STATE_RECV_HEADER:
	case STATE_RECV_FORM_NAME:
	case STATE_RECV_FORM_DATA:
	case STATE_RECV_ATTACHMENT_NAME:
	case STATE_RECV_ATTACHMENT_FILE:
	case STATE_RECV_ATTACHMENT_FILENAME:
	  // Receive and stash a known amount of data.
	  if (!recv_all_nonblock (conn, conn->current_size))
	    return;

	  switch (conn->dfa_state)
	    {
	    case STATE_RECV_URL:
	      set_url (conn, conn->current_data);
	      conn->dfa_state = STATE_RECV_INIT;
	      break;
	    case STATE_RECV_HEADER:
	      add_header (conn, conn->current_data);
	      FREE (conn->current_data);
	      conn->dfa_state = STATE_RECV_INIT;
	      break;
	    case STATE_RECV_FORM_NAME:
	      conn->stashed_string[0] = conn->current_data;
	      conn->dfa_state = STATE_RECV_SIZE;
	      conn->dfa_next_state = STATE_RECV_FORM_DATA;
	      break;
	    case STATE_RECV_FORM_DATA:
	      add_form_data (conn, conn->stashed_string[0],
	                     conn->current_data);
	      FREE (conn->stashed_string[0]);
	      FREE (conn->current_data);
	      conn->dfa_state = STATE_RECV_INIT;
	      break;
	    case STATE_RECV_ATTACHMENT_NAME:
	      conn->stashed_string[0] = conn->current_data;
	      conn->dfa_state = STATE_RECV_SIZE;
	      conn->dfa_next_state = STATE_RECV_ATTACHMENT_FILE;
	      break;
	    case STATE_RECV_ATTACHMENT_FILE:
	      conn->stashed_string[1] = conn->current_data;
	      conn->dfa_state = STATE_RECV_SIZE;
	      conn->dfa_next_state = STATE_RECV_ATTACHMENT_FILENAME;
	      break;
	    case STATE_RECV_ATTACHMENT_FILENAME:
	      conn->stashed_string[2] = conn->current_data;
	      conn->dfa_state = STATE_RECV_SIZE;
	      conn->dfa_next_state = STATE_RECV_ATTACHMENT_COMPLETE;
	      break;
	    default:
	      cc_log ("[%u:%u] Error: Broken state machine!",
	              conn->client_number, conn->job_number);
	      close_local_connection (conn);
	    }
	  conn->current_data = NULL;
	  break;

	case STATE_RECV_ATTACHMENT_COMPLETE:
	  add_form_attachment (conn, conn->stashed_string[0],
	                       conn->stashed_string[1],
	                       conn->current_size,
	                       conn->stashed_string[2]);
	  FREE (conn->stashed_string[0]);
	  FREE (conn->stashed_string[1]);
	  FREE (conn->stashed_string[2]);
	  conn->dfa_state = STATE_RECV_INIT;
	  break;

	case STATE_WAITING:
	case STATE_INPROGRESS:
	  /* We're waiting on an Internet connection.
	     We should never get here but it's harmless.

	     handle_completed_internet_connections() will change the
	     state to STATE_SEND_INIT and add the socket to FD_WRITE
	     when the time comes. */
	  return;

	case STATE_SEND_INIT:
	  if (!conn->send_buffer)
	    {
	      /* A send is not already in progress, so build another packet.  */
	      switch (get_response (conn, &conn->dr))
		{
		case D_RESPONSE_INCOMPLETE:
		  conn->dfa_next_state = STATE_RESET;
		  conn->current_data = x_strdup ("E");
		  conn->current_size = 1;
		  break;

		case D_RESPONSE_COMPLETE:
		  conn->dfa_next_state = STATE_RESET;
		  conn->current_data = x_strdup ("C");
		  conn->current_size = 1;
		  break;

		case D_REQUEST_FAILED:
		  conn->dfa_next_state = STATE_RESET;
		  conn->current_data = x_strdup ("F");
		  conn->current_size = 1;
		  break;

		case D_HTTP_RESULT_CODE:
		  conn->dfa_next_state = STATE_SEND_INIT;
		  conn->current_data = x_malloc (3);
		  conn->current_data[0] = 'R';
		  conn->current_data[1] = conn->dr->http_result_code & 0xFF;
		  conn->current_data[2] = (conn->dr->http_result_code & 0xFF00) >> 8;
		  FREE (conn->dr);
		  conn->current_size = 3;
		  break;

		case D_BODY:
		  conn->dfa_next_state = STATE_SEND_DATA_HEADER;
		  conn->current_data = x_malloc (9);
		  conn->current_data[0] = 'D';
		  conn->current_data[1] = conn->dr->body.headersize & 0xFF;
		  conn->current_data[2] = (conn->dr->body.headersize & 0xFF00) >> 8;
		  conn->current_data[3] = (conn->dr->body.headersize & 0xFF0000) >> 16;
		  conn->current_data[4] = (conn->dr->body.headersize & 0xFF000000) >> 24;
		  conn->current_data[5] = conn->dr->body.datasize & 0xFF;
		  conn->current_data[6] = (conn->dr->body.datasize & 0xFF00) >> 8;
		  conn->current_data[7] = (conn->dr->body.datasize & 0xFF0000) >> 16;
		  conn->current_data[8] = (conn->dr->body.datasize & 0xFF000000) >> 24;
		  conn->current_size = 9;
		  break;

		case D_ATTACHMENT:
		  conn->dfa_next_state = STATE_SEND_ATTACHMENT_HEADER;
		  conn->current_data = x_malloc (13);
		  conn->current_data[0] = 'A';
		  conn->current_data[1] = conn->dr->attachment.headersize & 0xFF;
		  conn->current_data[2] = (conn->dr->attachment.headersize & 0xFF00) >> 8;
		  conn->current_data[3] = (conn->dr->attachment.headersize & 0xFF0000) >> 16;
		  conn->current_data[4] = (conn->dr->attachment.headersize & 0xFF000000) >> 24;
		  int filenamesize = strlen (conn->dr->attachment.filename);
		  conn->current_data[5] = filenamesize & 0xFF;
		  conn->current_data[6] = (filenamesize & 0xFF00) >> 8;
		  conn->current_data[7] = (filenamesize & 0xFF0000) >> 16;
		  conn->current_data[8] = (filenamesize & 0xFF000000) >> 24;
		  int tmp_filenamesize = strlen (conn->dr->attachment.tmp_filename);
		  conn->current_data[9] = tmp_filenamesize & 0xFF;
		  conn->current_data[10] = (tmp_filenamesize & 0xFF00) >> 8;
		  conn->current_data[11] = (tmp_filenamesize & 0xFF0000) >> 16;
		  conn->current_data[12] = (tmp_filenamesize & 0xFF000000) >> 24;
		  conn->current_size = 13;
		  break;
		}
	    }

	  /* Send the packet in a resumable way. */
	  if (!send_all_nonblock (conn, conn->current_data, conn->current_size))
	    return;

	  /* The whole packet was sent.  */
	  conn->dfa_state = conn->dfa_next_state;
	  cc_log ("[%u:%u] sent '%c' to client.", conn->client_number,
	          conn->job_number, conn->current_data[0]);
	  FREE (conn->current_data);
	  break;

	case STATE_SEND_DATA_HEADER:
	  if (!send_all_nonblock (conn, conn->dr->body.headers,
	                          conn->dr->body.headersize))
	    return;
	  conn->dfa_state = STATE_SEND_DATA_BODY;
	  break;

	case STATE_SEND_DATA_BODY:
	  if (!send_all_nonblock (conn, conn->dr->body.data,
	                          conn->dr->body.datasize))
	    return;
	  conn->dfa_state = STATE_SEND_DR_DONE;
	  break;

	case STATE_SEND_ATTACHMENT_HEADER:
	  if (!send_all_nonblock (conn, conn->dr->attachment.headers,
	                          conn->dr->attachment.headersize))
	    return;
	  conn->dfa_state = STATE_SEND_ATTACHMENT_FAILNAME;
	  break;

	case STATE_SEND_ATTACHMENT_FAILNAME:
	  if (!send_all_nonblock (conn, conn->dr->attachment.filename,
	                          strlen (conn->dr->attachment.filename)))
	    return;
	  conn->dfa_state = STATE_SEND_ATTACHMENT_TMPFILENAME;
	  break;

	case STATE_SEND_ATTACHMENT_TMPFILENAME:
	  if (!send_all_nonblock (conn, conn->dr->attachment.tmp_filename,
	                          strlen (conn->dr->attachment.tmp_filename)))
	    return;
	  conn->dfa_state = STATE_SEND_DR_DONE;
	  break;

	case STATE_SEND_DR_DONE:
	  FREE (conn->dr);
	  conn->dfa_state = STATE_SEND_INIT;
	  break;

	case STATE_RESET:
	  cc_log ("[%u:%u] job complete.", conn->client_number,
	          conn->job_number);

	  /* Calculate the response times.
	     The counters here include incomplete requests,
	     but it probably doesn't matter.  */
	  struct timeval tv, diff;
	  gettimeofday (&tv, NULL);
	  timersub (&tv, &conn->request_time, &diff);
	  double time = diff.tv_sec + diff.tv_usec / 1000000.0;
	  if (conn->post)
	    {
	      if (lowest_post_response_time == 0
		  || lowest_post_response_time > time)
		lowest_post_response_time = time;
	      if (highest_post_response_time < time)
		highest_post_response_time = time;
	      average_post_response_time =
		((((post_request_counter-1) * average_post_response_time)
		  + time)
		 / post_request_counter);
	    }
	  else
	    {
	      if (lowest_get_response_time == 0
		  || lowest_get_response_time > time)
		lowest_get_response_time = time;
	      if (highest_get_response_time < time)
		highest_get_response_time = time;
	      average_get_response_time =
		((((get_request_counter-1) * average_get_response_time)
		  + time)
		 / get_request_counter);
	    }

	  /* Remove all the old state from this connection,
	     but keep it open so the client can reuse it.  */
	  FREE (conn->url);
	  cleanup_form (conn);
	  free_server_response (conn->response);
	  conn->response = NULL;
	  conn->job_number++;

	  /* Switch to read mode.  */
	  FD_CLR (conn->fd, &open_local_fds[FD_WRITE]);
	  conn->dfa_state = STATE_RECV_INIT;
	  break;
	}
    }
}

static void
setup_internet_request (struct internet_connection_state *iconn,
                        struct local_connection_state *lconn)
{
  x_curl_easy_setopt(iconn, CURLOPT_URL, (char *)lconn->url);
  x_curl_easy_setopt(iconn, CURLOPT_HTTPHEADER, lconn->header_list);
  if (lconn->post)
	x_curl_easy_setopt(iconn, CURLOPT_HTTPPOST, lconn->post);
  else
	x_curl_easy_setopt(iconn, CURLOPT_HTTPGET, (void *)1);
  iconn->response = calloc (1, sizeof(*iconn->response));
  x_curl_easy_setopt(iconn, CURLOPT_HEADERDATA, iconn->response);
  x_curl_easy_setopt(iconn, CURLOPT_WRITEDATA, iconn->response);
}

/* Match waiting local connections to free internet connections, set up the
   connection details, and launch curl.  */
static void
dispatch_jobs (void)
{
  struct local_connection_state *lconn;
  struct internet_connection_state *iconn;

  while (waiting_jobs
         && active_internet_connection_count < internet_pool_count)
    {
      /* Find waiting local connection.  */
      lconn = pop_queued_job ();

      /* Find inactive internet connection. */
      for (iconn = internet; iconn; iconn = iconn->next)
	if (!iconn->active)
	  break;

      /* Update both connection states.  */
      lconn->iconn = iconn;
      iconn->lconn = lconn;
      iconn->active = true;
      active_internet_connection_count++;
      internet_request_counter++;
      gettimeofday (&iconn->request_time, NULL);
      lconn->dfa_state = STATE_INPROGRESS;
      waiting_jobs--;

      setup_internet_request (iconn, lconn);

      /* And set it to go.  */
      curl_multi_add_handle (multi_handle, iconn->curl_handle);

      cc_log ("[%u:%u]<%u> Dispatched job to internet connection: %s",
              lconn->client_number, lconn->job_number,
              iconn->connection_number, lconn->url);
    }

  cc_log ("daemon has %d jobs left waiting", waiting_jobs);
}

/* For each completed curl handle, update our program state, and
   trigger do_local_comms to return the data to the client.  */
static void
handle_completed_internet_connections (void)
{
  int queued;

  do
    {
      CURLMsg *msg = curl_multi_info_read (multi_handle, &queued);

      if (!msg)
	break;
      else if (msg->msg == CURLMSG_DONE)
	{
	  CURL *eh = msg->easy_handle;

	  struct internet_connection_state *iconn;
	  for (iconn = internet; iconn; iconn = iconn->next)
	    if (iconn->curl_handle == eh)
	      break;

	  /* Clear any stashed data and close open files in the
	     reader function.  */
	  receive_cloud_response (NULL, 0, 0, iconn->response);

	  if (!iconn->lconn)
	    {
	      /* The client died while we were working.  */
	      // TODO clean up attachments?
	      cc_log ("<%u> internet request completed, but client already died",
	              iconn->connection_number);
	      goto reset_iconn;
	    }

	  if (msg->data.result == CURLE_OK)
	    cc_log ("[%u:%u]<%u> internet request completed",
		    iconn->lconn->client_number,
		    iconn->lconn->job_number,
		    iconn->connection_number);
	  else
	    cc_log ("[%u:%u]<%u> internet request failed: %s",
	            iconn->lconn->client_number,
	            iconn->lconn->job_number,
	            iconn->connection_number,
	            iconn->curl_error_buffer);

	  /* Calculate the response times.
	     The counters here include incomplete requests,
	     but it probably doesn't matter.  */
	  struct timeval tv, diff;
	  gettimeofday (&tv, NULL);
	  timersub (&tv, &iconn->request_time, &diff);
	  double time = diff.tv_sec + diff.tv_usec / 1000000.0;
	  if (iconn->lconn->post)
	    {
	      if (lowest_internet_post_response_time == 0
		  || lowest_internet_post_response_time > time)
		lowest_internet_post_response_time = time;
	      if (highest_internet_post_response_time < time)
		highest_internet_post_response_time = time;
	      average_internet_post_response_time =
		(((post_request_counter * average_internet_post_response_time)
		  + time)
		 / (post_request_counter + 1));
	      post_request_counter++;
	    }
	  else
	    {
	      if (lowest_internet_get_response_time == 0
		  || lowest_internet_get_response_time > time)
		lowest_internet_get_response_time = time;
	      if (highest_internet_get_response_time < time)
		highest_internet_get_response_time = time;
	      average_internet_get_response_time =
		(((get_request_counter * average_internet_get_response_time)
		  + time)
		 / (get_request_counter + 1));
	      get_request_counter++;
	    }

	  /* Pass the response data to the local connection state. */
	  iconn->lconn->response = iconn->response;
	  FD_SET (iconn->lconn->fd, &open_local_fds[FD_WRITE]);
	  iconn->lconn->dfa_state = STATE_SEND_INIT;

	  /* Reset this curl connection state, and return it to the pool.  */
	  iconn->lconn->iconn = NULL;
	reset_iconn:
	  iconn->response = NULL;
	  iconn->lconn = NULL;
	  iconn->active = false;
	  curl_multi_remove_handle (multi_handle, eh);
	  active_internet_connection_count--;
	}
      else
	cc_log ("WARNING: curl_multi_info_read did something unexpected!");
    }
  while (queued > 0);
}

static void
exit_handler (void)
{
  unlink (master_socket_path);

  cc_log ("Daemon Exiting (pid %d)", getpid());
}

static void
sigint_handler (int num __attribute__((unused)))
{
  cc_log ("Daemon killed with SIGINT.");
  exit (0);
}

static void
sigterm_handler (int num __attribute__((unused)))
{
  cc_log ("Daemon killed with SIGTERM.");
  exit (0);
}

static void
sigusr1_handler (int num __attribute__((unused)))
{
  cc_log ("Received SIGUSR1");
  fprintf (stderr, "cs daemon status\n");
  fprintf (stderr, "client connections: %u (%d still connected)\n",
           client_counter, active_clients);
  fprintf (stderr, "server connections: %d (%d currently in use)\n",
           internet_pool_count, active_internet_connection_count);

  struct local_connection_state *lconn;
  int awaiting_input = 0, receiving_input = 0, queued = 0,
      awaiting_server = 0, sending_response = 0;

  for (lconn = local; lconn; lconn = lconn->next)
    switch (lconn->dfa_state)
      {
      case STATE_RECV_INIT:
      case STATE_RESET:
	awaiting_input++;
	break;
      case STATE_RECV_SIZE:
      case STATE_RECV_URL:
      case STATE_RECV_HEADER:
      case STATE_RECV_FORM_NAME:
      case STATE_RECV_FORM_DATA:
      case STATE_RECV_ATTACHMENT_NAME:
      case STATE_RECV_ATTACHMENT_FILE:
      case STATE_RECV_ATTACHMENT_FILENAME:
      case STATE_RECV_ATTACHMENT_COMPLETE:
	receiving_input++;
	break;
      case STATE_WAITING:
	queued++;
	break;
      case STATE_INPROGRESS:
	awaiting_server++;
	break;
      case STATE_SEND_INIT:
      case STATE_SEND_DATA_HEADER:
      case STATE_SEND_DATA_BODY:
      case STATE_SEND_ATTACHMENT_HEADER:
      case STATE_SEND_ATTACHMENT_FAILNAME:
      case STATE_SEND_ATTACHMENT_TMPFILENAME:
      case STATE_SEND_DR_DONE:
	sending_response++;
	break;
      }

  fprintf (stderr, "local connection states:\n");
  fprintf (stderr, "idle=%d receiving=%d queued=%d internet=%d sending=%d\n",
           awaiting_input, receiving_input, queued, awaiting_server,
           sending_response);
  fprintf (stderr, "completed requests: GET=%u POST=%u\n",
           get_request_counter, post_request_counter);
  fprintf (stderr, "response times:  low   average   high\n");
  fprintf (stderr, "GET (internet)  %lf %lf %lf\n",
           lowest_internet_get_response_time,
           average_internet_get_response_time,
           highest_internet_get_response_time);
  fprintf (stderr, "GET (overall)   %lf %lf %lf\n",
           lowest_get_response_time, average_get_response_time,
           highest_get_response_time);
  fprintf (stderr, "POST (internet) %lf %lf %lf\n",
           lowest_internet_post_response_time,
           average_internet_post_response_time,
           highest_internet_post_response_time);
  fprintf (stderr, "POST (overall)  %lf %lf %lf\n",
           lowest_post_response_time, average_post_response_time,
           highest_post_response_time);
}

static void
sigusr2_handler (int num __attribute__((unused)))
{
  get_request_counter = 0;
  post_request_counter = 0;
  lowest_get_response_time = 0;
  lowest_internet_get_response_time = 0;
  lowest_post_response_time = 0;
  lowest_internet_post_response_time = 0;
  average_get_response_time = 0;
  average_internet_get_response_time = 0;
  average_post_response_time = 0;
  average_internet_post_response_time = 0;
  highest_get_response_time = 0;
  highest_internet_get_response_time = 0;
  highest_post_response_time = 0;
  highest_internet_post_response_time = 0;
}

int
daemon_main (bool force)
{
  cc_log ("Daemon Started on pid %d", getpid());

  /* First thing to do: create the Unix Domain Socket.
     If that doesn't work then another process probably got there first. */

  /* First create an unnamed socket ... */
  master_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (master_socket == -1)
    {
      cc_log ("Daemon could create Unix Domain Socket!");
      cc_log ("Daemon Exiting (pid %d)", getpid());
      exit (1);
    }
  fcntl(master_socket, F_SETFL, O_NONBLOCK);
  nfds = master_socket + 1;

  /* ... and then bind it to $CS_CACHE_DIR/daemon.<user>.<host>.<n>. */
  struct sockaddr_un addr = {AF_UNIX, ""};
  char *cwd = get_working_directory ();
  char *host_name = get_host_name ();
  char *socket_name = format ("daemon.%d.%s.%d", geteuid (), host_name,
                              LOCAL_PROTOCOL_REVISION);
  master_socket_path = format ("%s/%s", conf->cache_dir, socket_name);
  strcpy (addr.sun_path, socket_name);
  if (force)
    unlink (master_socket_path);
  while (chdir (conf->cache_dir) == -1
         || bind (master_socket, &addr, sizeof (addr)) == -1)
    {
      // We couldn't bind to the socket.
      if (errno == EADDRINUSE)
	{
	  // ... because somebody else has it already.
	  cc_log ("The named socket already exists.");

	  // If the socket is dead, we can delete it and try again.
	  // So, let's check if we can connect.
	  int tmp_socket = socket (AF_UNIX, SOCK_STREAM, 0);
	  if (tmp_socket != -1)
	    {
	      if (connect (tmp_socket, &addr, sizeof(addr)) == -1)
		{
		  // The named socket is dead.
		  cc_log ("Removing dead named socket.");
		  unlink (master_socket_path);
		  continue;
		}
	      else
		cc_log ("Another daemon is already running.");
	      close (tmp_socket);
	    }
	}
      else
	cc_log ("ERROR: Could not bind socket: %s", strerror(errno));
      // There's no point in retrying if we get this far.
      cc_log ("Daemon Exiting (pid %d)", getpid());
      exit (1);
    }
  if (chdir (cwd) == -1)
    { /* Silence warnings */ }
  free (cwd);
  free (host_name);
  free (socket_name);

  /* ... and set the socket to server mode.  */
  listen (master_socket, 50);
  FD_SET (master_socket, &open_local_fds[FD_READ]);
  cc_log ("Listening on socket at %s", master_socket_path);

  char *conn_count_s = getenv ("CS_DAEMON_CONNECTIONS");
  int conn_count = conn_count_s ? atoi (conn_count_s) : 8;

  /* Second, we initialize Libcurl (8 connections).  */
  int i;
  for (i = 0; i < conn_count; i++)
    if (init_new_easy_handle() == NULL)
      {
	cc_log ("Daemon Exiting (pid %d)", getpid());
	exit (1);
      }
  multi_handle = curl_multi_init();

  /* Register an exit handler to clean up the socket. */
  exitfn_add_nullary(exit_handler);
  signal (SIGINT, sigint_handler);
  signal (SIGTERM, sigterm_handler);
  signal (SIGPIPE, SIG_IGN);
  signal (SIGUSR1, sigusr1_handler);
  signal (SIGUSR2, sigusr2_handler);

  /* Main program loop.  */
  while (1)
    {
      /* curl_multi_wait would be nice here .... but we'll have to wait a
         while for that to make it into distros.  */
      fd_set readfds = open_local_fds[FD_READ];
      fd_set writefds = open_local_fds[FD_WRITE];
      fd_set exceptfds = open_local_fds[FD_READ];
      struct timeval timeout = {10*60, 0};  /* Ten minutes.  */
      if (active_internet_connection_count)
	{
	  int curl_nfds;
	  timeout.tv_sec = 0;
	  timeout.tv_usec = 100;
          curl_multi_fdset (multi_handle, &readfds, &writefds, &exceptfds,
                            &curl_nfds);
          if (curl_nfds > nfds)
            nfds = curl_nfds;
	}
      int fds = select (nfds, &readfds, &writefds, &exceptfds, &timeout);

      if (fds == 0 && active_internet_connection_count == 0
	  && local == NULL)
	{
	  /* Timeout.  */
	  cc_log ("No daemon activity for 10 minutes.");
	  break;
	}

      if (fds)
	{
	  if (master_socket != -1 && FD_ISSET(master_socket, &readfds))
	    accept_local_connections ();

	  /* Step through all the sockets with data, and handle whatever
	     needs doing.  */
	  struct local_connection_state *lconn, *next;

	  for (lconn = local; lconn; lconn = next)
	    {
	      /* This must be read first because do_local_comms might
	         delete lconn. */
	      next = lconn->next;

	      if (FD_ISSET(lconn->fd, &readfds)
		  || FD_ISSET(lconn->fd, &writefds)
		  || FD_ISSET(lconn->fd, &exceptfds))
		{
		  do_local_comms (lconn);
		  /* Warning: lconn might no longer exist. */
		}
	    }
	}

      if (waiting_jobs
	  && active_internet_connection_count < internet_pool_count)
	dispatch_jobs ();

      if (active_internet_connection_count > 0)
	{
	  int running_handles;
	  CURLMcode code = curl_multi_perform(multi_handle, &running_handles);
	  if (code != CURLM_OK)
	    {
	      cc_log ("Error: unhandled curl error in daemon: %s",
	              curl_multi_strerror (code));
	      exit(1);
	    }
	  if (active_internet_connection_count != running_handles)
	    {
	      handle_completed_internet_connections ();
	      if (waiting_jobs)
		dispatch_jobs ();
	    }
	}
    }

  return 0;
}
