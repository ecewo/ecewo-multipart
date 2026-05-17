# ecewo-multipart

Multipart/form-data parser plugin for [ecewo](https://github.com/savashn/ecewo). Handles file uploads and text fields from HTML forms, similar to [multer](https://github.com/expressjs/multer) for Express.js.

## Table of Contents

1. [Installation](#installation)
   1. [Building as a shared library](#building-as-a-shared-library-ffi-consumers)
2. [Quick Start](#quick-start)
3. [Usage](#usage)
   1. [As Per-Route Middleware](#as-per-route-middleware)
   2. [As Global Middleware](#as-global-middleware)
   3. [Accessing Text Fields](#accessing-text-fields)
   4. [Accessing Uploaded Files](#accessing-uploaded-files)
   5. [Iterating Over All Parsed Data](#iterating-over-all-parsed-data)
4. [Streaming Mode](#streaming-mode)
5. [Disk Storage](#disk-storage)
6. [Configuration](#configuration)
7. [API Reference](#api-reference)
   1. [Middleware](#middleware)
   2. [Configuration setters](#configuration-setters)
   3. [Lookup helpers](#lookup-helpers)
   4. [Container accessors](#container-accessors)
   5. [Field accessors](#field-accessors)
   6. [File accessors](#file-accessors)
8. [Memory Management](#memory-management)
9. [ABI & FFI Notes](#abi--ffi-notes)
10. [License](#license)

## Installation

Add to your `CMakeLists.txt`:

```cmake
ecewo_add(multipart)

target_link_libraries(app PRIVATE
  ecewo::ecewo
  ecewo::multipart
)
```

For disk-storage support, also add `fs`:

```cmake
ecewo_add(fs multipart)

target_link_libraries(app PRIVATE
  ecewo::ecewo
  ecewo::fs
  ecewo::multipart
)
```

### Building as a shared library (FFI consumers)

The plugin is FFI-friendly: only `ecewo_multipart_*` symbols are exported, public types are opaque, and there are no struct-layout contracts. Build a `.so` / `.dylib` / `.dll` with:

```sh
cmake -B build -DECEWO_MULTIPART_BUILD_SHARED=ON
cmake --build build
```

Combine with `-DECEWO_BUILD_SHARED=ON -DECEWO_FS_BUILD_SHARED=ON` to get fully shared dependencies as well.

## Quick Start

```c
#include "ecewo.h"
#include "ecewo-multipart.h"

static void upload_handler(ecewo_request_t *req, ecewo_response_t *res) {
  const char *username = ecewo_multipart_get_field_value(req, "username");
  const ecewo_multipart_file_t *avatar = ecewo_multipart_get_file(req, "avatar");

  if (!avatar) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "No file uploaded");
    return;
  }

  char *msg = ecewo_sprintf(ecewo_req_arena(req),
      "User: %s, File: %s (%zu bytes)",
      username ? username : "anonymous",
      ecewo_multipart_file_filename(avatar),
      ecewo_multipart_file_size(avatar));

  ecewo_send_text(res, ECEWO_OK, msg);
}

int main(void) {
  ecewo_app_t *app = ecewo_create();
  ECEWO_POST(app, "/upload", ecewo_multipart, upload_handler);
  ecewo_listen(app, 3000);
  return 0;
}
```

## Usage

### As Per-Route Middleware

Apply `ecewo_multipart` to specific routes that handle file uploads:

```c
ECEWO_POST(app, "/upload",  ecewo_multipart, upload_handler);
ECEWO_POST(app, "/profile", ecewo_multipart, profile_handler);
```

### As Global Middleware

Apply it to every request. Non-multipart requests pass through unchanged:

```c
ecewo_use(app, NULL, ecewo_multipart);
```

### Accessing Text Fields

```c
static void handler(ecewo_request_t *req, ecewo_response_t *res) {
  const char *name  = ecewo_multipart_get_field_value(req, "name");
  const char *email = ecewo_multipart_get_field_value(req, "email");
  // ...
}
```

Or, when you need length-aware access (binary-safe values):

```c
const ecewo_multipart_field_t *f = ecewo_multipart_get_field(req, "name");
if (f) {
  const char *v = ecewo_multipart_field_value(f);
  size_t      n = ecewo_multipart_field_value_len(f);
}
```

### Accessing Uploaded Files

```c
static void handler(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "avatar");
  if (!file) return;

  ecewo_multipart_file_fieldname(file);  // "avatar"
  ecewo_multipart_file_filename(file);   // "photo.png"
  ecewo_multipart_file_mimetype(file);   // "image/png"
  ecewo_multipart_file_data(file);       // binary content, or NULL if streamed
  ecewo_multipart_file_size(file);       // size in bytes
  ecewo_multipart_file_path(file);       // disk path, or NULL if in memory only
}
```

### Iterating Over All Parsed Data

```c
static void handler(ecewo_request_t *req, ecewo_response_t *res) {
  ecewo_multipart_t *mp = ecewo_multipart_get(req);
  if (!mp) return;

  for (size_t i = 0; i < ecewo_multipart_field_count(mp); i++) {
    const ecewo_multipart_field_t *f = ecewo_multipart_field_at(mp, i);
    // ecewo_multipart_field_name(f), ecewo_multipart_field_value(f), ...
  }

  for (size_t i = 0; i < ecewo_multipart_file_count(mp); i++) {
    const ecewo_multipart_file_t *f = ecewo_multipart_file_at(mp, i);
    // ecewo_multipart_file_filename(f), ecewo_multipart_file_size(f), ...
  }
}
```

## Streaming Mode

For large uploads, place `ecewo_body_stream` before `ecewo_multipart` so chunks are processed as they arrive instead of buffering the entire body in memory:

```c
ECEWO_POST(app, "/upload", ecewo_body_stream, ecewo_multipart, upload_handler);
```

The handler code is identical — `ecewo_multipart_get_file()`, `ecewo_multipart_get_field()`, and `ecewo_multipart_get()` work the same way. The middleware automatically detects streaming mode and writes file chunks straight to disk as they arrive. Text fields are still kept in RAM (they are typically small).

When disk storage is configured (see below), streamed files are staged in the destination directory and finalized with an atomic rename — never re-copied.

## Disk Storage

Configure `ecewo_multipart_disk(app, dir)` to have uploaded files written to disk automatically. Requires the `fs` plugin.

```c
#include "ecewo.h"
#include "ecewo-fs.h"
#include "ecewo-multipart.h"

static void upload_handler(ecewo_request_t *req, ecewo_response_t *res) {
  const ecewo_multipart_file_t *file = ecewo_multipart_get_file(req, "document");
  if (!file) return;

  // ecewo_multipart_file_path(file) holds the saved path, e.g. "uploads/1772458930-0.pdf"
  char *msg = ecewo_sprintf(ecewo_req_arena(req),
      "Saved to: %s", ecewo_multipart_file_path(file));
  ecewo_send_text(res, ECEWO_OK, msg);
}

int main(void) {
  fs_init();

  ecewo_app_t *app = ecewo_create();
  ecewo_multipart_disk(app, "uploads");

  ECEWO_POST(app, "/upload", ecewo_body_stream, ecewo_multipart, upload_handler);

  ecewo_listen(app, 3000);
  return 0;
}
```

Saved files are named `<timestamp>-<counter>.<ext>` to avoid collisions; the extension is sanitized (alphanumeric only, max 16 chars) to defend against malicious filenames.

## Configuration

All configuration is **per-app**, stored via `ecewo_set_app_data`, so multi-app processes don't share state. Call setters before `ecewo_listen()`.

```c
ecewo_app_t *app = ecewo_create();

ecewo_multipart_disk(app, "uploads");                  // disk destination
ecewo_multipart_threshold(app, 2 * 1024 * 1024);       // 2 MB RAM ceiling (default)
ecewo_multipart_max_size(app, 100 * 1024 * 1024);      // reject bodies > 100 MB (413)
ecewo_multipart_max_parts(app, 100);                   // limit fields + files per request

ECEWO_POST(app, "/upload", ecewo_body_stream, ecewo_multipart, handler);
ecewo_listen(app, 3000);
```

## API Reference

All identifiers are declared in `ecewo-multipart.h`.

### Middleware

```c
void ecewo_multipart(ecewo_request_t *req, ecewo_response_t *res, ecewo_next_t next);
```

Parses `multipart/form-data` request bodies. Non-multipart requests pass through to the next handler unchanged. Works in both buffered mode and streaming mode (when `ecewo_body_stream` runs first).

### Configuration setters

All setters return `0` on success and `-1` on failure (typically: `app` is `NULL`, or the per-app config could not be allocated). Each setter creates the per-app configuration on first call.

| Setter | Description |
|---|---|
| `int ecewo_multipart_disk(ecewo_app_t *app, const char *dest)` | Set the directory where uploaded files are saved. Pass `NULL` to disable disk storage. |
| `int ecewo_multipart_threshold(ecewo_app_t *app, size_t max_ram_bytes)` | Maximum cumulative file data kept in RAM for buffered uploads (default: 2 MB). Streaming mode ignores this. |
| `int ecewo_multipart_max_size(ecewo_app_t *app, size_t max_bytes)` | Reject request bodies larger than `max_bytes` with HTTP 413. `0` (default) = unlimited. |
| `int ecewo_multipart_max_parts(ecewo_app_t *app, size_t n)` | Maximum number of parts (fields + files) per request; extras are silently discarded. Default: 100. |

### Lookup helpers

| Function | Description |
|---|---|
| `const ecewo_multipart_field_t *ecewo_multipart_get_field(const ecewo_request_t *req, const char *name)` | Return the first text field with the given name, or `NULL`. |
| `const char *ecewo_multipart_get_field_value(const ecewo_request_t *req, const char *name)` | Convenience: returns the value string of the named field, or `NULL` if absent. |
| `const ecewo_multipart_file_t *ecewo_multipart_get_file(const ecewo_request_t *req, const char *fieldname)` | Return the first uploaded file with the given field name, or `NULL`. |

### Container accessors

| Function | Description |
|---|---|
| `ecewo_multipart_t *ecewo_multipart_get(const ecewo_request_t *req)` | Return the parsed container, or `NULL` if the middleware did not run / nothing was parsed. |
| `size_t ecewo_multipart_field_count(const ecewo_multipart_t *mp)` | Number of text fields. |
| `size_t ecewo_multipart_file_count(const ecewo_multipart_t *mp)` | Number of file uploads. |
| `const ecewo_multipart_field_t *ecewo_multipart_field_at(const ecewo_multipart_t *mp, size_t i)` | Indexed access; `NULL` if `i` is out of range. |
| `const ecewo_multipart_file_t *ecewo_multipart_file_at(const ecewo_multipart_t *mp, size_t i)` | Indexed access; `NULL` if `i` is out of range. |

### Field accessors

| Function | Description |
|---|---|
| `const char *ecewo_multipart_field_name(const ecewo_multipart_field_t *f)` | Field name. |
| `const char *ecewo_multipart_field_value(const ecewo_multipart_field_t *f)` | Field value (always null-terminated). |
| `size_t ecewo_multipart_field_value_len(const ecewo_multipart_field_t *f)` | Value length, in bytes (use with `_field_value` for binary-safe data). |

### File accessors

| Function | Description |
|---|---|
| `const char *ecewo_multipart_file_fieldname(const ecewo_multipart_file_t *f)` | Form field name (e.g. `"avatar"`). |
| `const char *ecewo_multipart_file_filename(const ecewo_multipart_file_t *f)` | Client-supplied filename (e.g. `"photo.png"`). |
| `const char *ecewo_multipart_file_mimetype(const ecewo_multipart_file_t *f)` | Client-supplied MIME type, or `"application/octet-stream"` if absent. |
| `const uint8_t *ecewo_multipart_file_data(const ecewo_multipart_file_t *f)` | Binary content, or `NULL` when the upload was streamed straight to disk. |
| `size_t ecewo_multipart_file_size(const ecewo_multipart_file_t *f)` | Total size in bytes. |
| `const char *ecewo_multipart_file_path(const ecewo_multipart_file_t *f)` | On-disk path when disk storage is used, otherwise `NULL`. |

## Memory Management

Every piece of parsed data — including text-field values, file metadata, and (in buffered mode) file contents — is allocated in the per-request arena, and is freed automatically when the response is sent. Callers don't need to free anything.

Pointers returned by the accessors are valid until the response is sent; do **not** retain them past that point.

For uploads streamed to disk:

- **With `ecewo_multipart_disk`**: each file is staged in the destination directory and atomically renamed when the part completes — no cleanup required.
- **Without `ecewo_multipart_disk`** (streaming mode only): the upload lands in a temp file under the OS temp dir at `ecewo_multipart_file_path(f)`. The path remains valid after the response is sent; delete the file yourself with `fs_unlink()` or `remove()` once you're done with it.

## ABI & FFI Notes

The public surface is designed for stable ABI and clean cross-language bindings:

- All structs are opaque (`ecewo_multipart_t`, `ecewo_multipart_field_t`, `ecewo_multipart_file_t`). Access is through accessor functions only — field reordering or new fields will never break callers.
- Only `ecewo_multipart_*` symbols are exported from the shared library (default visibility is hidden, set via `C_VISIBILITY_PRESET hidden`).
- `SOVERSION` is the project major version (`0` today); link against `libecewo-multipart.so.0` for binary stability across patch and minor releases.
- The header has no compile-time dependency on platform types beyond `<stddef.h>` and `<stdint.h>`, plus ecewo's own opaque handles.

To verify the export set yourself after a shared build:

```sh
nm -D --defined-only build/libecewo-multipart.so | awk '$2=="T"' | sort
```

## License

MIT
