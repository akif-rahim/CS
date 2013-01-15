#include "hashutil.h"

int cloud_initialize(void);
void cloud_exit(const char *reason, int status) __attribute__((noreturn));

bool cloud_offline_mode ();
bool cloud_cache_get (const char *object_file, const char *stderr_file,
                      int *exit_status);
int cloud_cache_exit_status (void);

void cloud_hook_start_overall_timer(void);
void cloud_hook_stop_overall_timer(void);
void cloud_hook_starting_direct_mode(void);
void cloud_hook_starting_preprocessor_mode(void);
void cloud_hook_ending_preprocessor_mode(void);
void cloud_hook_starting_compiler_execution(void);
void cloud_hook_ending_compiler_execution();
void cloud_hook_cpp_hash(const char *source_hash);
void cloud_hook_object_path(const char *object_path);
void cloud_hook_source_file(const char *source_file, struct file_hash *hash);
void cloud_hook_include_file(const char *source_file, struct file_hash *hash);
void cloud_hook_preprocessed_file(const char *file);
void cloud_hook_reset_includes(void);
void cloud_hook_object_file (const char *cache_file);
void cloud_hook_stderr_file (const char *cache_file);
void cloud_hook_direct_mode_autodisabled (const char *reason);
void cloud_hook_fork_successful (void);

enum result_type {
  RT_NOT_SET,
  RT_DIRECT_CACHE_HIT,
  RT_PREPROCESSOR_CACHE_HIT,
  RT_CLOUD_CACHE_HIT,
  RT_LOCAL_COMPILE
};
void cloud_hook_record_result_type (enum result_type);
