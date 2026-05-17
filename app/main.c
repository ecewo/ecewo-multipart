#include "ecewo.h"
#include "ecewo-multipart.h"
#include "ecewo-fs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define UPLOADS_DIR "uploads"

static const char *UPLOAD_FORM =
  "<!DOCTYPE html>"
  "<html>"
  "<head><title>Photo Upload</title>"
  "<style>"
  "body { font-family: sans-serif; max-width: 500px; margin: 60px auto; }"
  "h1 { color: #333; }"
  "input[type=file] { display: block; margin: 16px 0; }"
  "button { background: #0070f3; color: white; border: none;"
  "  padding: 10px 24px; border-radius: 4px; cursor: pointer; font-size: 15px; }"
  "button:hover { background: #0051a8; }"
  "</style>"
  "</head>"
  "<body>"
  "<h1>Upload a Photo</h1>"
  "<form action=\"/upload\" method=\"post\" enctype=\"multipart/form-data\">"
  "  <label>Select an image:</label>"
  "  <input type=\"file\" name=\"photo\" accept=\"image/*\" required>"
  "  <button type=\"submit\">Upload</button>"
  "</form>"
  "</body>"
  "</html>";

static int is_image(const char *mimetype) {
  if (!mimetype) return 0;
  return strncmp(mimetype, "image/", 6) == 0;
}

static void get_form(ecewo_request_t *req, ecewo_response_t *res) {
  (void)req;
  ecewo_send_html(res, ECEWO_OK, UPLOAD_FORM);
}

static void post_upload(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *photo = ecewo_multipart_get_file(req, "photo");

  if (!photo) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "No file uploaded. Use the 'photo' field.");
    return;
  }

  const char *mimetype = ecewo_multipart_file_mimetype(photo);
  const char *filename = ecewo_multipart_file_filename(photo);

  if (!is_image(mimetype)) {
    char *msg = ecewo_sprintf(ecewo_req_arena(req),
        "Rejected: '%s' is not an image (got %s).",
        filename, mimetype ? mimetype : "unknown type");
    ecewo_send_text(res, ECEWO_UNSUPPORTED_MEDIA_TYPE, msg);
    return;
  }

  const char *path = ecewo_multipart_file_path(photo);

  char *body = ecewo_sprintf(ecewo_req_arena(req),
      "Uploaded successfully!\n"
      "  Filename : %s\n"
      "  Type     : %s\n"
      "  Size     : %zu bytes\n"
      "  Saved to : %s\n",
      filename,
      mimetype,
      ecewo_multipart_file_size(photo),
      path ? path : "(memory only)");

  ecewo_send_text(res, ECEWO_OK, body);
}

int main(void) {
  mkdir(UPLOADS_DIR, 0755);

  fs_init();

  ecewo_app_t *app = ecewo_create();
  if (!app) {
    fprintf(stderr, "Failed to create app\n");
    return -1;
  }

  if (ecewo_multipart_disk(app, UPLOADS_DIR) != 0) {
    fprintf(stderr, "Failed to configure multipart disk storage\n");
    return -1;
  }

  ECEWO_GET(app, "/", get_form);
  ECEWO_POST(app, "/upload", ecewo_body_stream, ecewo_multipart, post_upload);

  printf("Listening on http://localhost:8000\n");
  printf("Uploads saved to ./%s/\n", UPLOADS_DIR);

  ecewo_listen(app, 8000);
  return 0;
}
