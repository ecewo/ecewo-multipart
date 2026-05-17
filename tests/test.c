#include "ecewo.h"
#include "ecewo-mock.h"
#include "ecewo-multipart.h"
#include "ecewo-fs.h"
#include "tester.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ============================================================================
// Route Handlers
// ============================================================================

void handler_single_field(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_field_t *field = ecewo_multipart_get_field(req, "username");
  if (!field) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "missing field");
    return;
  }
  ecewo_send_text(res, ECEWO_OK, ecewo_multipart_field_value(field));
}

void handler_single_file(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "avatar");
  if (!file) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "missing file");
    return;
  }
  char *response = ecewo_sprintf(ecewo_req_arena(req), "name=%s;type=%s;size=%zu",
                                 ecewo_multipart_file_filename(file),
                                 ecewo_multipart_file_mimetype(file),
                                 ecewo_multipart_file_size(file));
  ecewo_send_text(res, ECEWO_OK, response);
}

void handler_mixed(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_field_t *uf = ecewo_multipart_get_field(req, "username");
  const ecewo_multipart_field_t *ef = ecewo_multipart_get_field(req, "email");
  const ecewo_multipart_file_t *fl = ecewo_multipart_get_file(req, "avatar");

  if (!uf || !ef || !fl) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "missing data");
    return;
  }

  char *response = ecewo_sprintf(ecewo_req_arena(req),
                                 "user=%s;email=%s;file=%s;size=%zu",
                                 ecewo_multipart_field_value(uf),
                                 ecewo_multipart_field_value(ef),
                                 ecewo_multipart_file_filename(fl),
                                 ecewo_multipart_file_size(fl));
  ecewo_send_text(res, ECEWO_OK, response);
}

void handler_multiple_files(ecewo_request_t *req, ecewo_response_t *res) {
  ecewo_multipart_t *mp = ecewo_multipart_get(req);
  if (!mp) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "no multipart");
    return;
  }

  char *response = ecewo_sprintf(ecewo_req_arena(req), "fields=%zu;files=%zu",
                                 ecewo_multipart_field_count(mp),
                                 ecewo_multipart_file_count(mp));
  ecewo_send_text(res, ECEWO_OK, response);
}

void handler_passthrough(ecewo_request_t *req, ecewo_response_t *res) {
  ecewo_multipart_t *mp = ecewo_multipart_get(req);
  if (mp) {
    ecewo_send_text(res, ECEWO_OK, "has multipart");
  } else {
    ecewo_send_text(res, ECEWO_OK, "no multipart");
  }
}

void handler_file_content(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "doc");
  if (!file) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "missing file");
    return;
  }
  size_t size = ecewo_multipart_file_size(file);
  const uint8_t *data = ecewo_multipart_file_data(file);
  char *content = ecewo_alloc(ecewo_req_arena(req), size + 1);
  memcpy(content, data, size);
  content[size] = '\0';
  ecewo_send_text(res, ECEWO_OK, content);
}

void handler_binary_content(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "blob");
  if (!file) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "missing file");
    return;
  }
  size_t size = ecewo_multipart_file_size(file);
  const uint8_t *data = ecewo_multipart_file_data(file);

  char *hex = ecewo_alloc(ecewo_req_arena(req), size * 2 + 1);
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < size; i++) {
    hex[i * 2] = digits[(data[i] >> 4) & 0xF];
    hex[i * 2 + 1] = digits[data[i] & 0xF];
  }
  hex[size * 2] = '\0';
  ecewo_send_text(res, ECEWO_OK, hex);
}

// ============================================================================
// Tests
// ============================================================================

typedef struct {
  const void *data;
  size_t len;
} Part;

// Binary-safe multipart body builder. Returns body bytes via out_len; caller frees.
static uint8_t *build_multipart_body_bin(const char *boundary,
                                         const Part *parts,
                                         size_t part_count,
                                         size_t *out_len) {
  size_t bnd_len = strlen(boundary);
  size_t total = 0;
  for (size_t i = 0; i < part_count; i++)
    total += 2 + bnd_len + 2 + parts[i].len; // "--" + boundary + "\r\n" + part
  total += 2 + bnd_len + 2 + 2; // closing "--" + boundary + "--\r\n"

  uint8_t *body = malloc(total);
  if (!body)
    return NULL;

  size_t off = 0;
  for (size_t i = 0; i < part_count; i++) {
    body[off++] = '-';
    body[off++] = '-';
    memcpy(body + off, boundary, bnd_len);
    off += bnd_len;
    body[off++] = '\r';
    body[off++] = '\n';
    memcpy(body + off, parts[i].data, parts[i].len);
    off += parts[i].len;
  }
  body[off++] = '-';
  body[off++] = '-';
  memcpy(body + off, boundary, bnd_len);
  off += bnd_len;
  body[off++] = '-';
  body[off++] = '-';
  body[off++] = '\r';
  body[off++] = '\n';

  *out_len = off;
  return body;
}

// String wrapper: each part is a C string; total length stays in body.
static char *build_multipart_body(const char *boundary, const char *parts[], size_t part_count) {
  Part *bin_parts = malloc(part_count * sizeof(Part));
  if (!bin_parts)
    return NULL;
  for (size_t i = 0; i < part_count; i++) {
    bin_parts[i].data = parts[i];
    bin_parts[i].len = strlen(parts[i]);
  }
  size_t total;
  uint8_t *body = build_multipart_body_bin(boundary, bin_parts, part_count, &total);
  free(bin_parts);
  if (!body)
    return NULL;
  // Re-allocate +1 for null terminator so callers can treat it as a C string.
  char *out = realloc(body, total + 1);
  if (!out) {
    free(body);
    return NULL;
  }
  out[total] = '\0';
  return out;
}

int test_single_text_field(void) {
  const char *boundary = "----TestBoundary123";

  const char *parts[] = {
    "Content-Disposition: form-data; name=\"username\"\r\n"
    "\r\n"
    "johndoe\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 1);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----TestBoundary123" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/single-field",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("johndoe", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_single_file_upload(void) {
  const char *boundary = "----TestBoundary456";

  const char *parts[] = {
    "Content-Disposition: form-data; name=\"avatar\"; filename=\"photo.png\"\r\n"
    "Content-Type: image/png\r\n"
    "\r\n"
    "FAKE_PNG_DATA_HERE\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 1);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----TestBoundary456" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/single-file",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("name=photo.png;type=image/png;size=18", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_mixed_fields_and_files(void) {
  const char *boundary = "----TestBoundary789";

  const char *parts[] = {
    "Content-Disposition: form-data; name=\"username\"\r\n"
    "\r\n"
    "johndoe\r\n",

    "Content-Disposition: form-data; name=\"email\"\r\n"
    "\r\n"
    "john@example.com\r\n",

    "Content-Disposition: form-data; name=\"avatar\"; filename=\"pic.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n"
    "\r\n"
    "JPEG_DATA\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 3);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----TestBoundary789" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/mixed",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("user=johndoe;email=john@example.com;file=pic.jpg;size=9", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_multiple_files(void) {
  const char *boundary = "----TestBoundaryMulti";

  const char *parts[] = {
    "Content-Disposition: form-data; name=\"title\"\r\n"
    "\r\n"
    "My Upload\r\n",

    "Content-Disposition: form-data; name=\"file1\"; filename=\"doc.pdf\"\r\n"
    "Content-Type: application/pdf\r\n"
    "\r\n"
    "PDF_CONTENT\r\n",

    "Content-Disposition: form-data; name=\"file2\"; filename=\"img.png\"\r\n"
    "Content-Type: image/png\r\n"
    "\r\n"
    "PNG_CONTENT\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 3);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----TestBoundaryMulti" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/multiple-files",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("fields=1;files=2", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_non_multipart_passthrough(void) {
  MockHeaders headers[] = {
    { "Content-Type", "application/json" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/passthrough",
    .body = "{\"key\":\"value\"}",
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("no multipart", res.body);

  free_request(&res);
  RETURN_OK();
}

int test_no_content_type_passthrough(void) {
  MockParams params = {
    .method = MOCK_POST,
    .path = "/passthrough",
    .body = "some data"
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("no multipart", res.body);

  free_request(&res);
  RETURN_OK();
}

int test_file_content_preserved(void) {
  const char *boundary = "----ContentTest";
  const char *file_content = "Hello, this is file content with special chars: <>&\"'!@#$%";

  char part[512];
  snprintf(part, sizeof(part),
           "Content-Disposition: form-data; name=\"doc\"; filename=\"test.txt\"\r\n"
           "Content-Type: text/plain\r\n"
           "\r\n"
           "%s\r\n",
           file_content);

  const char *parts[] = { part };
  char *body = build_multipart_body(boundary, parts, 1);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----ContentTest" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/file-content",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR(file_content, res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_default_mimetype(void) {
  const char *boundary = "----DefaultMime";

  const char *parts[] = {
    "Content-Disposition: form-data; name=\"avatar\"; filename=\"data.bin\"\r\n"
    "\r\n"
    "binary_stuff\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 1);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----DefaultMime" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/single-file",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("name=data.bin;type=application/octet-stream;size=12", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

// ============================================================================
// Streaming Mode Tests
// ============================================================================

void handler_stream_mixed(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_field_t *uf = ecewo_multipart_get_field(req, "username");
  const ecewo_multipart_file_t *fl = ecewo_multipart_get_file(req, "avatar");

  if (!uf || !fl) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "missing data");
    return;
  }

  char *response = ecewo_sprintf(ecewo_req_arena(req),
                                 "user=%s;file=%s;size=%zu",
                                 ecewo_multipart_field_value(uf),
                                 ecewo_multipart_file_filename(fl),
                                 ecewo_multipart_file_size(fl));
  ecewo_send_text(res, ECEWO_OK, response);
}

int test_streaming_mixed(void) {
  const char *boundary = "----StreamBnd";

  const char *parts[] = {
    "Content-Disposition: form-data; name=\"username\"\r\n"
    "\r\n"
    "streamuser\r\n",

    "Content-Disposition: form-data; name=\"avatar\"; filename=\"stream.png\"\r\n"
    "Content-Type: image/png\r\n"
    "\r\n"
    "STREAM_PNG_DATA\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 2);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----StreamBnd" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/stream-mixed",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("user=streamuser;file=stream.png;size=15", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

// ============================================================================
// Disk Storage Tests
// ============================================================================

void handler_disk_upload(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "doc");
  if (!file) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "missing file");
    return;
  }

  const char *path = ecewo_multipart_file_path(file);
  if (!path) {
    ecewo_send_text(res, ECEWO_INTERNAL_SERVER_ERROR, "no path set");
    return;
  }

  char *response = ecewo_sprintf(ecewo_req_arena(req), "path=%s;size=%zu",
                                 path, ecewo_multipart_file_size(file));
  ecewo_send_text(res, ECEWO_OK, response);
}

int test_disk_storage(void) {
  const char *boundary = "----DiskTest";

  const char *parts[] = {
    "Content-Disposition: form-data; name=\"doc\"; filename=\"report.txt\"\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "disk file content\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 1);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----DiskTest" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/disk-upload",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_NOT_NULL(res.body);

  ASSERT_TRUE(strncmp(res.body, "path=test_uploads/", 18) == 0);
  ASSERT_NOT_NULL(strstr(res.body, ".txt"));
  ASSERT_NOT_NULL(strstr(res.body, "size=17"));

  free(body);
  free_request(&res);
  RETURN_OK();
}

// ============================================================================
// Binary Content Round-trip
// ============================================================================

int test_binary_content_roundtrip(void) {
  const char *boundary = "----BinaryBnd";

  // Bytes 0x01..0xFF (skip 0x00 — the mock transport uses strlen on the body,
  // so an embedded NUL would truncate the request). Still covers 0x0D, 0x0A,
  // 0x2D ('-'), and all non-ASCII high bytes that could fool a naive parser.
  uint8_t payload[255];
  for (size_t i = 0; i < 255; i++)
    payload[i] = (uint8_t)(i + 1);

  const char *header = "Content-Disposition: form-data; name=\"blob\"; filename=\"data.bin\"\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "\r\n";
  size_t header_len = strlen(header);
  const char *trailer = "\r\n";
  size_t trailer_len = 2;

  size_t part_len = header_len + sizeof(payload) + trailer_len;
  uint8_t *part_buf = malloc(part_len);
  memcpy(part_buf, header, header_len);
  memcpy(part_buf + header_len, payload, sizeof(payload));
  memcpy(part_buf + header_len + sizeof(payload), trailer, trailer_len);

  Part parts[] = { { part_buf, part_len } };
  size_t body_len;
  uint8_t *body = build_multipart_body_bin(boundary, parts, 1, &body_len);
  (void)body_len;

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----BinaryBnd" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/binary",
    .body = (const char *)body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_NOT_NULL(res.body);

  // Build expected hex string for the 255-byte payload.
  char expected[511];
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 255; i++) {
    expected[i * 2] = digits[(payload[i] >> 4) & 0xF];
    expected[i * 2 + 1] = digits[payload[i] & 0xF];
  }
  expected[510] = '\0';
  ASSERT_EQ_STR(expected, res.body);

  free(part_buf);
  free(body);
  free_request(&res);
  RETURN_OK();
}

// ============================================================================
// Malformed Input Tests
// ============================================================================

int test_missing_content_disposition(void) {
  const char *boundary = "----NoCD";

  // Part has no Content-Disposition header — parser should skip it.
  const char *parts[] = {
    "Content-Type: text/plain\r\n"
    "\r\n"
    "orphan data\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 1);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----NoCD" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/single-field",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(400, res.status_code);
  ASSERT_EQ_STR("missing field", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_missing_name_attribute(void) {
  const char *boundary = "----NoName";

  // Content-Disposition without name= and without filename= — nothing usable.
  const char *parts[] = {
    "Content-Disposition: form-data\r\n"
    "\r\n"
    "anonymous\r\n"
  };

  char *body = build_multipart_body(boundary, parts, 1);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----NoName" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/single-field",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(400, res.status_code);
  ASSERT_EQ_STR("missing field", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_truncated_body_no_closing_boundary(void) {
  // Hand-built body with the closing "--boundary--\r\n" deliberately omitted.
  // Parser needs the trailing boundary to terminate the last part, so the part
  // never lands in the parsed container.
  const char *truncated = "------TruncBnd\r\n"
                          "Content-Disposition: form-data; name=\"username\"\r\n"
                          "\r\n"
                          "johndoe\r\n";

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----TruncBnd" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/single-field",
    .body = truncated,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(400, res.status_code);
  ASSERT_EQ_STR("missing field", res.body);

  free_request(&res);
  RETURN_OK();
}

int test_wrong_boundary_in_header(void) {
  // Body uses one boundary; Content-Type header advertises a different one.
  // Parser should find no first delimiter and return an empty container.
  const char *parts[] = {
    "Content-Disposition: form-data; name=\"username\"\r\n"
    "\r\n"
    "johndoe\r\n"
  };
  char *body = build_multipart_body("----ActualBnd", parts, 1);

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----DifferentBnd" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/single-field",
    .body = body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(400, res.status_code);
  ASSERT_EQ_STR("missing field", res.body);

  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_empty_multipart_body(void) {
  // Valid multipart Content-Type but zero-length body — parser short-circuits to 400.
  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----EmptyBnd" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/passthrough",
    .body = "",
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(400, res.status_code);
  ASSERT_EQ_STR("Empty multipart body", res.body);

  free_request(&res);
  RETURN_OK();
}

int test_payload_too_large(void) {
  // Build a body larger than ecewo_multipart_max_size (set to 4096 in runner).
  const char *boundary = "----TooBigBnd";

  // 8KB payload — bigger than the configured 4096 limit.
  size_t payload_len = 8192;
  char *payload = malloc(payload_len);
  memset(payload, 'A', payload_len);

  const char *header = "Content-Disposition: form-data; name=\"blob\"; filename=\"big.bin\"\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "\r\n";
  size_t header_len = strlen(header);
  const char *trailer = "\r\n";

  size_t part_len = header_len + payload_len + 2;
  uint8_t *part_buf = malloc(part_len);
  memcpy(part_buf, header, header_len);
  memcpy(part_buf + header_len, payload, payload_len);
  memcpy(part_buf + header_len + payload_len, trailer, 2);

  Part parts[] = { { part_buf, part_len } };
  size_t body_len;
  uint8_t *body = build_multipart_body_bin(boundary, parts, 1, &body_len);
  (void)body_len;

  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data; boundary=----TooBigBnd" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/passthrough",
    .body = (const char *)body,
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(413, res.status_code);

  free(payload);
  free(part_buf);
  free(body);
  free_request(&res);
  RETURN_OK();
}

int test_multipart_without_boundary_param(void) {
  // Content-Type advertises multipart but has no boundary= parameter.
  // Middleware can't parse — should fall through; handler sees no multipart context.
  MockHeaders headers[] = {
    { "Content-Type", "multipart/form-data" }
  };

  MockParams params = {
    .method = MOCK_POST,
    .path = "/passthrough",
    .body = "irrelevant",
    .headers = headers,
    .header_count = 1
  };

  MockResponse res = request(&params);
  ASSERT_EQ(200, res.status_code);
  ASSERT_EQ_STR("no multipart", res.body);

  free_request(&res);
  RETURN_OK();
}
