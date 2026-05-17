# Form with Text Fields and Files

Handle a form submission that includes both text fields and file uploads, like a user profile form.

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(form_with_files C)

include(FetchContent)

FetchContent_Declare(
  ecewo
  GIT_REPOSITORY https://github.com/ecewo/ecewo.git
  GIT_TAG v4
)
FetchContent_MakeAvailable(ecewo)

ecewo_add(multipart)

add_executable(form_with_files main.c)
target_link_libraries(form_with_files PRIVATE ecewo::ecewo ecewo::multipart)
```

## main.c

```c
#include "ecewo.h"
#include "ecewo-multipart.h"
#include <stdio.h>

static void profile_handler(ecewo_request_t *req, ecewo_response_t *res) {
  const char *username = ecewo_multipart_get_field_value(req, "username");
  const char *email    = ecewo_multipart_get_field_value(req, "email");
  const char *bio      = ecewo_multipart_get_field_value(req, "bio");
  const ecewo_multipart_file_t *avatar = ecewo_multipart_get_file(req, "avatar");

  if (!username || !email) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "Username and email are required");
    return;
  }

  printf("New profile:\n");
  printf("  Username: %s\n", username);
  printf("  Email: %s\n", email);
  if (bio) printf("  Bio: %s\n", bio);
  if (avatar)
    printf("  Avatar: %s (%zu bytes)\n",
           ecewo_multipart_file_filename(avatar),
           ecewo_multipart_file_size(avatar));

  char *msg = ecewo_sprintf(ecewo_req_arena(req),
      "Profile created for %s", username);
  ecewo_send_text(res, ECEWO_OK, msg);
}

static void gallery_handler(ecewo_request_t *req, ecewo_response_t *res) {
  const char *title = ecewo_multipart_get_field_value(req, "title");
  ecewo_multipart_t *data = ecewo_multipart_get(req);

  if (!data || ecewo_multipart_file_count(data) == 0) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "No images uploaded");
    return;
  }

  printf("Gallery: %s\n", title ? title : "(untitled)");

  size_t n = ecewo_multipart_file_count(data);
  for (size_t i = 0; i < n; i++) {
    const ecewo_multipart_file_t *img = ecewo_multipart_file_at(data, i);
    printf("  [%zu] %s (%s, %zu bytes)\n",
           i + 1,
           ecewo_multipart_file_filename(img),
           ecewo_multipart_file_mimetype(img),
           ecewo_multipart_file_size(img));
  }

  char *msg = ecewo_sprintf(ecewo_req_arena(req),
      "Uploaded %zu image(s)", n);
  ecewo_send_text(res, ECEWO_OK, msg);
}

int main(void) {
  ecewo_app_t *app = ecewo_create();
  if (!app) {
    fprintf(stderr, "Failed to create app\n");
    return 1;
  }

  ECEWO_POST(app, "/profile", ecewo_multipart, profile_handler);
  ECEWO_POST(app, "/gallery", ecewo_multipart, gallery_handler);

  printf("Listening on http://localhost:3000\n");
  ecewo_listen(app, 3000);
  return 0;
}
```

## Testing

Profile with avatar:

```bash
curl -F "username=johndoe" \
     -F "email=john@example.com" \
     -F "bio=Hello world" \
     -F "avatar=@photo.jpg" \
     http://localhost:3000/profile
```

Multiple file upload:

```bash
curl -F "title=Vacation" \
     -F "photos=@beach.jpg" \
     -F "photos=@sunset.png" \
     -F "photos=@hotel.jpg" \
     http://localhost:3000/gallery
```
