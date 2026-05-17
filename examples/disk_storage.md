# Disk Storage

Save uploaded files to disk automatically, similar to multer's `diskStorage`. Requires the `fs` plugin.

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(disk_storage C)

include(FetchContent)

FetchContent_Declare(
  ecewo
  GIT_REPOSITORY https://github.com/ecewo/ecewo.git
  GIT_TAG v4
)
FetchContent_MakeAvailable(ecewo)

ecewo_add(fs multipart)

add_executable(disk_storage main.c)
target_link_libraries(disk_storage PRIVATE ecewo::ecewo ecewo::fs ecewo::multipart)
```

## main.c

```c
#include "ecewo.h"
#include "ecewo-multipart.h"
#include "ecewo-fs.h"
#include <stdio.h>
#include <sys/stat.h>

static void upload_handler(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "document");

  if (!file) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "No file uploaded");
    return;
  }

  // ecewo_multipart_file_path(file) is set automatically by the multipart middleware
  // e.g. "uploads/1772458930-0.pdf"
  char *msg = ecewo_sprintf(ecewo_req_arena(req),
      "Saved %s to %s (%zu bytes)",
      ecewo_multipart_file_filename(file),
      ecewo_multipart_file_path(file),
      ecewo_multipart_file_size(file));

  ecewo_send_text(res, ECEWO_OK, msg);
}

static void multi_upload_handler(ecewo_request_t *req, ecewo_response_t *res) {
  ecewo_multipart_t *data = ecewo_multipart_get(req);

  if (!data || ecewo_multipart_file_count(data) == 0) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "No files uploaded");
    return;
  }

  // All files are saved to the configured directory
  size_t n = ecewo_multipart_file_count(data);
  for (size_t i = 0; i < n; i++) {
    const ecewo_multipart_file_t *f = ecewo_multipart_file_at(data, i);
    printf("Saved: %s -> %s\n",
           ecewo_multipart_file_filename(f),
           ecewo_multipart_file_path(f));
  }

  char *msg = ecewo_sprintf(ecewo_req_arena(req),
      "Saved %zu file(s) to uploads/", n);
  ecewo_send_text(res, ECEWO_OK, msg);
}

int main(void) {
  // Create uploads directory
  mkdir("uploads", 0755);

  // Initialize fs plugin
  if (fs_init() != 0) {
    fprintf(stderr, "Failed to initialize fs\n");
    return 1;
  }

  ecewo_app_t *app = ecewo_create();
  if (!app) {
    fprintf(stderr, "Failed to create app\n");
    return 1;
  }

  // Configure disk storage (per app)
  ecewo_multipart_disk(app, "uploads");

  ECEWO_POST(app, "/upload", ecewo_multipart, upload_handler);
  ECEWO_POST(app, "/batch",  ecewo_multipart, multi_upload_handler);

  printf("Listening on http://localhost:3000\n");
  printf("Files will be saved to ./uploads/\n");
  ecewo_listen(app, 3000);
  return 0;
}
```

## Testing

Single file:

```bash
curl -F "document=@report.pdf" http://localhost:3000/upload
```

Response:

```
Saved report.pdf to uploads/1772458930-0.pdf (51200 bytes)
```

Multiple files:

```bash
curl -F "files=@doc1.pdf" -F "files=@doc2.pdf" http://localhost:3000/batch
```
