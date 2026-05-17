// Copyright 2026 Savas Sahin <savashn@proton.me>

// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef ECEWO_MULTIPART_H
#define ECEWO_MULTIPART_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "ecewo.h"
#include "ecewo-multipart-export.h"

/**
 * Opaque container for all parsed multipart data on a request.
 * Owned by the per-request arena; valid until the response is sent.
 */
typedef struct ecewo_multipart_s ecewo_multipart_t;

/** Opaque parsed text field. */
typedef struct ecewo_multipart_field_s ecewo_multipart_field_t;

/** Opaque parsed file upload. */
typedef struct ecewo_multipart_file_s ecewo_multipart_file_t;

// ---------------------------------------------------------------------------
// CONFIGURATION (per app)
//
// Each setter scopes config to a single ecewo_app_t instance via
// ecewo_set_app_data, so multi-app processes don't share state.
// Call before ecewo_listen(); 0 on success, -1 on error.
// ---------------------------------------------------------------------------

/** Save uploaded files to dest. NULL = keep uploads in RAM (or in temp files). */
ECEWO_MULTIPART_EXPORT int ecewo_multipart_disk(ecewo_app_t *app, const char *dest);

/** Maximum RAM threshold for buffered uploads (default: 2MB). Streaming mode ignores this. */
ECEWO_MULTIPART_EXPORT int ecewo_multipart_threshold(ecewo_app_t *app, size_t max_ram_bytes);

/** Maximum total upload body size in bytes (default: 0 = unlimited). 413 if exceeded. */
ECEWO_MULTIPART_EXPORT int ecewo_multipart_max_size(ecewo_app_t *app, size_t max_bytes);

/** Maximum number of parts (fields + files) per request (default: 100). Extra parts ignored. */
ECEWO_MULTIPART_EXPORT int ecewo_multipart_max_parts(ecewo_app_t *app, size_t n);

// ---------------------------------------------------------------------------
// MIDDLEWARE
//
// Parses multipart/form-data bodies. Non-multipart requests pass through.
// Place after ecewo_body_stream to enable true streaming (chunks written
// straight to disk); otherwise the full body is buffered first.
// ---------------------------------------------------------------------------

ECEWO_MULTIPART_EXPORT void ecewo_multipart(ecewo_request_t *req,
                                            ecewo_response_t *res,
                                            ecewo_next_t next);

// ---------------------------------------------------------------------------
// ACCESSING PARSED DATA
//
// All returned pointers live on the per-request arena.
// ---------------------------------------------------------------------------

/** Return the multipart container, or NULL if the middleware did not run / nothing was parsed. */
ECEWO_MULTIPART_EXPORT ecewo_multipart_t *ecewo_multipart_get(const ecewo_request_t *req);

ECEWO_MULTIPART_EXPORT size_t ecewo_multipart_field_count(const ecewo_multipart_t *mp);
ECEWO_MULTIPART_EXPORT size_t ecewo_multipart_file_count(const ecewo_multipart_t *mp);

/** Indexed access. Returns NULL if i is out of range. */
ECEWO_MULTIPART_EXPORT const ecewo_multipart_field_t *ecewo_multipart_field_at(const ecewo_multipart_t *mp, size_t i);
ECEWO_MULTIPART_EXPORT const ecewo_multipart_file_t *ecewo_multipart_file_at(const ecewo_multipart_t *mp, size_t i);

/** Lookup helpers: return the first matching field/file, or NULL. */
ECEWO_MULTIPART_EXPORT const ecewo_multipart_field_t *ecewo_multipart_get_field(const ecewo_request_t *req, const char *name);
ECEWO_MULTIPART_EXPORT const ecewo_multipart_file_t *ecewo_multipart_get_file(const ecewo_request_t *req, const char *fieldname);

/** Convenience: returns the value of the named field, or NULL if absent. */
ECEWO_MULTIPART_EXPORT const char *ecewo_multipart_get_field_value(const ecewo_request_t *req, const char *name);

// Field accessors
ECEWO_MULTIPART_EXPORT const char *ecewo_multipart_field_name(const ecewo_multipart_field_t *f);
ECEWO_MULTIPART_EXPORT const char *ecewo_multipart_field_value(const ecewo_multipart_field_t *f);
ECEWO_MULTIPART_EXPORT size_t ecewo_multipart_field_value_len(const ecewo_multipart_field_t *f);

// File accessors
ECEWO_MULTIPART_EXPORT const char *ecewo_multipart_file_fieldname(const ecewo_multipart_file_t *f);
ECEWO_MULTIPART_EXPORT const char *ecewo_multipart_file_filename(const ecewo_multipart_file_t *f);
ECEWO_MULTIPART_EXPORT const char *ecewo_multipart_file_mimetype(const ecewo_multipart_file_t *f);
/** Returns NULL when the upload was streamed straight to disk (no RAM copy). */
ECEWO_MULTIPART_EXPORT const uint8_t *ecewo_multipart_file_data(const ecewo_multipart_file_t *f);
ECEWO_MULTIPART_EXPORT size_t ecewo_multipart_file_size(const ecewo_multipart_file_t *f);
/** Path on disk when disk storage is configured; NULL otherwise. */
ECEWO_MULTIPART_EXPORT const char *ecewo_multipart_file_path(const ecewo_multipart_file_t *f);

#ifdef __cplusplus
}
#endif

#endif
