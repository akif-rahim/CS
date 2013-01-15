typedef int daemon_handle;

int launch_daemon (void);
int daemon_main (bool force);
daemon_handle connect_to_daemon (void);
void close_daemon (daemon_handle dh);

int set_daemon_url (daemon_handle dh, char *url);
int add_daemon_header (daemon_handle dh, const char *header);
int add_daemon_header (daemon_handle dh, const char *header);
int add_daemon_form_data (daemon_handle dh, const char *name,
                          const char *data);
int add_daemon_form_attachment (daemon_handle dh, const char *name,
                                struct stashed_file *sf, const char *filename);

enum daemon_response_codes
{
  D_REQUEST_FAILED,	 /* Total network connection failure.  */
  D_RESPONSE_INCOMPLETE, /* No more responses, but there should be.  */
  D_RESPONSE_COMPLETE,   /* No more responses, because we've reached the end.  */
  D_HTTP_RESULT_CODE,
  D_BODY,                /* The headers, and body data for a message part. */
  D_ATTACHMENT           /* A message part that was saved to file.  */
};

union daemon_responses
{
  /* D_HTTP_RESULT_CODE */
  int http_result_code;

  /* D_BODY */
  struct
  {
    char *headers;
    size_t headersize;
    char *data;
    size_t datasize;
  } body;

  /* D_ATTACHMENT */
  struct
  {
    char *headers;
    size_t headersize;
    char *filename;
    char *tmp_filename;
  } attachment;
};

int request_daemon_response (daemon_handle dh);
enum daemon_response_codes get_daemon_response (daemon_handle dh,
                                                union daemon_responses **dr_ptr);
void flush_daemon_response (daemon_handle dh);
