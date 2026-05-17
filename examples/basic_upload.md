# Basic File Upload

A simple file upload server that accepts a file and returns its metadata.

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(basic_upload C)

include(FetchContent)

FetchContent_Declare(
  ecewo
  GIT_REPOSITORY https://github.com/ecewo/ecewo.git
  GIT_TAG v4
)
FetchContent_MakeAvailable(ecewo)

ecewo_add(multipart)

add_executable(basic_upload main.c)
target_link_libraries(basic_upload PRIVATE ecewo::ecewo ecewo::multipart)
```

## main.c

```c
#include "ecewo.h"
#include "ecewo-multipart.h"
#include <stdio.h>

static void upload_handler(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "avatar");

  if (!file) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "No file uploaded");
    return;
  }

  char *msg = ecewo_sprintf(ecewo_req_arena(req),
      "Received: %s (%s, %zu bytes)",
      ecewo_multipart_file_filename(file),
      ecewo_multipart_file_mimetype(file),
      ecewo_multipart_file_size(file));

  ecewo_send_text(res, ECEWO_OK, msg);
}

int main(void) {
  ecewo_app_t *app = ecewo_create();
  if (!app) {
    fprintf(stderr, "Failed to create app\n");
    return 1;
  }

  ECEWO_POST(app, "/upload", ecewo_multipart, upload_handler);

  printf("Listening on http://localhost:3000\n");
  ecewo_listen(app, 3000);
  return 0;
}
```

## Testing

```bash
curl -F "avatar=@photo.png" http://localhost:3000/upload
```

Response:

```
Received: photo.png (image/png, 24576 bytes)
```
