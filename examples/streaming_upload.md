# Streaming Upload

For large file uploads, use `ecewo_body_stream` before `ecewo_multipart` so chunks are written straight to disk as they arrive instead of being buffered in memory first.

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(streaming_upload C)

include(FetchContent)

FetchContent_Declare(
  ecewo
  GIT_REPOSITORY https://github.com/ecewo/ecewo.git
  GIT_TAG v4
)
FetchContent_MakeAvailable(ecewo)

ecewo_add(fs multipart)

add_executable(streaming_upload main.c)
target_link_libraries(streaming_upload PRIVATE ecewo::ecewo ecewo::fs ecewo::multipart)
```

## main.c

```c
#include "ecewo.h"
#include "ecewo-multipart.h"
#include "ecewo-fs.h"
#include <stdio.h>
#include <sys/stat.h>

static void upload_handler(ecewo_request_t *req, ecewo_response_t *res) {
  const char *description = ecewo_multipart_get_field_value(req, "description");
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "file");

  if (!file) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "No file uploaded");
    return;
  }

  const char *filename = ecewo_multipart_file_filename(file);
  const char *path     = ecewo_multipart_file_path(file);
  size_t      size     = ecewo_multipart_file_size(file);

  printf("Upload: %s (%zu bytes)\n", filename, size);
  if (description) printf("Description: %s\n", description);
  if (path)        printf("Saved to: %s\n", path);

  char *msg = ecewo_sprintf(ecewo_req_arena(req),
      "Received %s (%zu bytes)", filename, size);
  ecewo_send_text(res, ECEWO_OK, msg);
}

int main(void) {
  mkdir("uploads", 0755);

  if (fs_init() != 0) {
    fprintf(stderr, "Failed to initialize fs\n");
    return 1;
  }

  ecewo_app_t *app = ecewo_create();
  if (!app) {
    fprintf(stderr, "Failed to create app\n");
    return 1;
  }

  ecewo_multipart_disk(app, "uploads");

  // ecewo_body_stream must come before ecewo_multipart in the middleware chain
  ECEWO_POST(app, "/upload", ecewo_body_stream, ecewo_multipart, upload_handler);

  printf("Listening on http://localhost:3000\n");
  ecewo_listen(app, 3000);
  return 0;
}
```

The handler code is identical to the buffered mode. The `ecewo_multipart` middleware automatically detects streaming mode and writes file chunks straight to disk. `ecewo_multipart_get_file()`, `ecewo_multipart_get_field()`, and `ecewo_multipart_get()` all work the same way.

## Testing

Upload a large file:

```bash
curl -F "description=Large video file" \
     -F "file=@video.mp4" \
     http://localhost:3000/upload
```
