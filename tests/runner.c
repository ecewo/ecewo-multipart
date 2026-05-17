#include "ecewo.h"
#include "ecewo-mock.h"
#include "ecewo-multipart.h"
#include "ecewo-fs.h"
#include "tester.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

int g_test_pass = 0;
int g_test_fail = 0;
int g_test_skip = 0;

void handler_single_field(ecewo_request_t *req, ecewo_response_t *res);
void handler_single_file(ecewo_request_t *req, ecewo_response_t *res);
void handler_mixed(ecewo_request_t *req, ecewo_response_t *res);
void handler_multiple_files(ecewo_request_t *req, ecewo_response_t *res);
void handler_passthrough(ecewo_request_t *req, ecewo_response_t *res);
void handler_file_content(ecewo_request_t *req, ecewo_response_t *res);
void handler_stream_mixed(ecewo_request_t *req, ecewo_response_t *res);
void handler_disk_upload(ecewo_request_t *req, ecewo_response_t *res);
void handler_binary_content(ecewo_request_t *req, ecewo_response_t *res);

int test_single_text_field(void);
int test_single_file_upload(void);
int test_mixed_fields_and_files(void);
int test_multiple_files(void);
int test_non_multipart_passthrough(void);
int test_no_content_type_passthrough(void);
int test_file_content_preserved(void);
int test_default_mimetype(void);
int test_streaming_mixed(void);
int test_disk_storage(void);
int test_binary_content_roundtrip(void);
int test_missing_content_disposition(void);
int test_missing_name_attribute(void);
int test_truncated_body_no_closing_boundary(void);
int test_wrong_boundary_in_header(void);
int test_empty_multipart_body(void);
int test_payload_too_large(void);
int test_multipart_without_boundary_param(void);

// Wipe any leftover files from previous runs so test_disk_storage is deterministic.
static void clean_upload_dir(const char *path) {
  DIR *d = opendir(path);
  if (!d)
    return;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
    unlink(full);
  }
  closedir(d);
}

static void setup_routes(ecewo_app_t *app) {
  ecewo_multipart_disk(app, "test_uploads");
  // Cap large enough for every happy-path test (largest body ~700B with the
  // binary payload), small enough that test_payload_too_large can exceed it.
  ecewo_multipart_max_size(app, 4096);

  ECEWO_POST(app, "/single-field", ecewo_multipart, handler_single_field);
  ECEWO_POST(app, "/single-file", ecewo_multipart, handler_single_file);
  ECEWO_POST(app, "/mixed", ecewo_multipart, handler_mixed);
  ECEWO_POST(app, "/multiple-files", ecewo_multipart, handler_multiple_files);
  ECEWO_POST(app, "/passthrough", ecewo_multipart, handler_passthrough);
  ECEWO_POST(app, "/file-content", ecewo_multipart, handler_file_content);
  ECEWO_POST(app, "/stream-mixed", ecewo_body_stream, ecewo_multipart, handler_stream_mixed);
  ECEWO_POST(app, "/disk-upload", ecewo_multipart, handler_disk_upload);
  ECEWO_POST(app, "/binary", ecewo_multipart, handler_binary_content);
}

int main(void) {
  mkdir("test_uploads", 0755);
  clean_upload_dir("test_uploads");
  fs_init();

  if (mock_init(setup_routes) != 0)
    return 1;

  RUN_TEST(test_single_text_field);
  RUN_TEST(test_single_file_upload);
  RUN_TEST(test_mixed_fields_and_files);
  RUN_TEST(test_multiple_files);
  RUN_TEST(test_non_multipart_passthrough);
  RUN_TEST(test_no_content_type_passthrough);
  RUN_TEST(test_file_content_preserved);
  RUN_TEST(test_default_mimetype);
  RUN_TEST(test_streaming_mixed);
  RUN_TEST(test_disk_storage);
  RUN_TEST(test_binary_content_roundtrip);
  RUN_TEST(test_missing_content_disposition);
  RUN_TEST(test_missing_name_attribute);
  RUN_TEST(test_truncated_body_no_closing_boundary);
  RUN_TEST(test_wrong_boundary_in_header);
  RUN_TEST(test_empty_multipart_body);
  RUN_TEST(test_payload_too_large);
  RUN_TEST(test_multipart_without_boundary_param);

  TEST_SUMMARY();

  mock_cleanup();
  return TEST_EXIT_CODE();
}
