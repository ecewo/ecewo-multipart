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

#include "ecewo-multipart.h"
#include "ecewo-fs.h"
#include "ecewo.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Internal data layout
// ---------------------------------------------------------------------------

struct ecewo_multipart_field_s {
  char *name;
  char *value; // null-terminated
  size_t value_len;
};

struct ecewo_multipart_file_s {
  char *fieldname;
  char *filename;
  char *mimetype;
  uint8_t *data; // NULL when streamed to disk
  size_t size;
  char *path; // set when disk storage is used
};

struct ecewo_multipart_s {
  ecewo_multipart_field_t *fields;
  size_t field_count;
  ecewo_multipart_file_t *files;
  size_t file_count;
};

// ---------------------------------------------------------------------------
// Per-app configuration
// ---------------------------------------------------------------------------

#ifndef MAX_RAM_THRESHOLD
#define MAX_RAM_THRESHOLD ((size_t)2 * 1024 * 1024)
#endif

typedef struct {
  char *disk_dest; // arena-allocated, NULL = no disk storage
  size_t ram_threshold;
  size_t max_upload_size; // 0 = unlimited
  size_t max_parts;
} multipart_config_t;

// Address used as a stable key for ecewo_set_app_data — unique per process,
// no coordination with other plugins needed.
static char g_multipart_cfg_key;

static multipart_config_t *cfg_get_or_create(ecewo_app_t *app) {
  if (!app)
    return NULL;
  multipart_config_t *cfg = ecewo_get_app_data(app, &g_multipart_cfg_key);
  if (cfg)
    return cfg;
  cfg = ecewo_alloc(ecewo_app_arena(app), sizeof(*cfg));
  if (!cfg)
    return NULL;
  cfg->disk_dest = NULL;
  cfg->ram_threshold = MAX_RAM_THRESHOLD;
  cfg->max_upload_size = 0;
  cfg->max_parts = 100;
  ecewo_set_app_data(app, &g_multipart_cfg_key, cfg);
  return cfg;
}

static const multipart_config_t *cfg_for_request(const ecewo_request_t *req) {
  multipart_config_t *cfg = ecewo_get_app_data(ecewo_req_app((ecewo_request_t *)req),
                                               &g_multipart_cfg_key);
  return cfg ? cfg : NULL;
}

// Process-wide name-uniquifier for generated filenames
static _Atomic uint64_t g_file_counter = 0;

#define MULTIPART_CTX_KEY "ecewo_multipart"
#define STREAM_MP_KEY "ecewo_multipart_stream"

// ---------------------------------------------------------------------------
// Configuration setters
// ---------------------------------------------------------------------------

int ecewo_multipart_disk(ecewo_app_t *app, const char *dest) {
  if (!app)
    return -1;
  multipart_config_t *cfg = cfg_get_or_create(app);
  if (!cfg)
    return -1;
  if (!dest) {
    cfg->disk_dest = NULL;
    return 0;
  }
  char *copy = ecewo_strdup(ecewo_app_arena(app), dest);
  if (!copy)
    return -1;
  cfg->disk_dest = copy;
  return 0;
}

int ecewo_multipart_threshold(ecewo_app_t *app, size_t max_ram_bytes) {
  multipart_config_t *cfg = cfg_get_or_create(app);
  if (!cfg)
    return -1;
  cfg->ram_threshold = max_ram_bytes;
  return 0;
}

int ecewo_multipart_max_size(ecewo_app_t *app, size_t max_bytes) {
  multipart_config_t *cfg = cfg_get_or_create(app);
  if (!cfg)
    return -1;
  cfg->max_upload_size = max_bytes;
  return 0;
}

int ecewo_multipart_max_parts(ecewo_app_t *app, size_t n) {
  multipart_config_t *cfg = cfg_get_or_create(app);
  if (!cfg)
    return -1;
  cfg->max_parts = n;
  return 0;
}

// ---------------------------------------------------------------------------
// Portability helpers
// ---------------------------------------------------------------------------

#ifdef _WIN32
static void *memmem(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len) {
  if (needle_len == 0)
    return (void *)haystack;
  if (needle_len > haystack_len)
    return NULL;

  const unsigned char *h = (const unsigned char *)haystack;
  const unsigned char *n = (const unsigned char *)needle;
  size_t limit = haystack_len - needle_len;

  for (size_t i = 0; i <= limit; i++) {
    if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0)
      return (void *)(h + i);
  }

  return NULL;
}
#endif

// Extract the boundary string from a Content-Type header value.
static char *extract_boundary(ecewo_arena_t *arena, const char *content_type) {
  if (!content_type)
    return NULL;

  while (*content_type == ' ')
    content_type++;

  const char *prefix = "multipart/form-data";
  size_t prefix_len = strlen(prefix);

  if (strlen(content_type) < prefix_len)
    return NULL;

  for (size_t i = 0; i < prefix_len; i++) {
    if (tolower((unsigned char)content_type[i]) != prefix[i])
      return NULL;
  }

  const char *p = content_type + prefix_len;
  while (*p) {
    if (*p == ';') {
      p++;
      while (*p == ' ' || *p == '\t')
        p++;

      if (strncmp(p, "boundary=", 9) == 0) {
        p += 9;

        if (*p == '"') {
          p++;
          const char *end = strchr(p, '"');
          if (!end)
            return NULL;
          size_t len = (size_t)(end - p);
          if (len == 0 || len > 256)
            return NULL;
          char *boundary = ecewo_alloc(arena, len + 1);
          memcpy(boundary, p, len);
          boundary[len] = '\0';
          return boundary;
        }

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '\r' && *p != '\n')
          p++;
        size_t len = (size_t)(p - start);
        if (len == 0 || len > 256)
          return NULL;
        char *boundary = ecewo_alloc(arena, len + 1);
        memcpy(boundary, start, len);
        boundary[len] = '\0';
        return boundary;
      }
    } else {
      p++;
    }
  }

  return NULL;
}

// Extract a parameter value from a header line, e.g. name="value" or filename="file".
static char *extract_param(ecewo_arena_t *arena, const char *header, const char *param_name) {
  if (!header || !param_name)
    return NULL;

  size_t param_len = strlen(param_name);
  const char *p = header;

  while (*p) {
    const char *candidate = p;

    while (*candidate == ' ' || *candidate == '\t' || *candidate == ';')
      candidate++;

    if (strncmp(candidate, param_name, param_len) == 0 && candidate[param_len] == '=') {
      candidate += param_len + 1;

      if (*candidate == '"') {
        candidate++;
        const char *end = strchr(candidate, '"');
        if (!end)
          return NULL;
        size_t len = (size_t)(end - candidate);
        char *value = ecewo_alloc(arena, len + 1);
        memcpy(value, candidate, len);
        value[len] = '\0';
        return value;
      }

      const char *start = candidate;
      while (*candidate && *candidate != ';' && *candidate != ' ' && *candidate != '\r' && *candidate != '\n')
        candidate++;
      size_t len = (size_t)(candidate - start);
      if (len == 0)
        return NULL;
      char *value = ecewo_alloc(arena, len + 1);
      memcpy(value, start, len);
      value[len] = '\0';
      return value;
    }

    p++;
  }

  return NULL;
}

// Find a header line (case-insensitive key match) within a part's header block.
static char *find_part_header(ecewo_arena_t *arena, const char *headers, size_t headers_len, const char *header_name) {
  size_t name_len = strlen(header_name);
  const char *p = headers;
  const char *end = headers + headers_len;

  while (p < end) {
    const char *line_end = memmem(p, (size_t)(end - p), "\r\n", 2);
    if (!line_end)
      line_end = end;

    size_t line_len = (size_t)(line_end - p);

    if (line_len > name_len + 1) {
      bool match = true;
      for (size_t i = 0; i < name_len; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)header_name[i])) {
          match = false;
          break;
        }
      }
      if (match && p[name_len] == ':') {
        const char *val = p + name_len + 1;
        while (val < line_end && (*val == ' ' || *val == '\t'))
          val++;
        size_t val_len = (size_t)(line_end - val);
        char *result = ecewo_alloc(arena, val_len + 1);
        memcpy(result, val, val_len);
        result[val_len] = '\0';
        return result;
      }
    }

    if (line_end + 2 <= end)
      p = line_end + 2;
    else
      break;
  }

  return NULL;
}

static const char *get_tmp_dir(void) {
#ifdef _WIN32
  const char *tmp = getenv("TEMP");
  if (!tmp)
    tmp = getenv("TMP");
  if (!tmp)
    tmp = "C:\\Temp";
  return tmp;
#else
  const char *tmp = getenv("TMPDIR");
  return tmp ? tmp : "/tmp";
#endif
}

static char *generate_tmp_path(ecewo_arena_t *arena) {
  uint64_t count = atomic_fetch_add(&g_file_counter, 1);
  time_t now = time(NULL);
  return ecewo_sprintf(arena, "%s/ecewo-mp-%lu-%lu-%d.tmp",
                       get_tmp_dir(),
                       (unsigned long)now, (unsigned long)count,
                       (int)getpid());
}

// Validate a file extension; returns ".ext" or "".
static const char *safe_extension(ecewo_arena_t *arena, const char *filename) {
  const char *dot = NULL;
  for (const char *p = filename; *p; p++) {
    if (*p == '.')
      dot = p;
  }
  if (!dot)
    return "";

  const char *ext = dot + 1;
  size_t len = strlen(ext);

  if (len == 0 || len > 16)
    return "";

  for (size_t i = 0; i < len; i++) {
    if (!isalnum((unsigned char)ext[i]))
      return "";
  }

  char *safe = ecewo_alloc(arena, len + 2);
  safe[0] = '.';
  memcpy(safe + 1, ext, len);
  safe[len + 1] = '\0';
  return safe;
}

// ---------------------------------------------------------------------------
// Buffered (non-streaming) parser
// ---------------------------------------------------------------------------

static ecewo_multipart_t *parse_multipart_body(ecewo_arena_t *arena,
                                               size_t max_parts,
                                               const uint8_t *body,
                                               size_t body_len,
                                               const char *boundary) {
  ecewo_multipart_t *data = ecewo_alloc(arena, sizeof(ecewo_multipart_t));
  memset(data, 0, sizeof(*data));

  size_t fields_cap = 4;
  size_t files_cap = 4;
  data->fields = ecewo_alloc(arena, fields_cap * sizeof(ecewo_multipart_field_t));
  data->files = ecewo_alloc(arena, files_cap * sizeof(ecewo_multipart_file_t));

  size_t bnd_len = strlen(boundary);

  size_t first_delim_len = 2 + bnd_len;
  char *first_delim = ecewo_alloc(arena, first_delim_len + 1);
  first_delim[0] = '-';
  first_delim[1] = '-';
  memcpy(first_delim + 2, boundary, bnd_len);
  first_delim[first_delim_len] = '\0';

  size_t delim_len = 4 + bnd_len;
  char *delim = ecewo_alloc(arena, delim_len + 1);
  delim[0] = '\r';
  delim[1] = '\n';
  delim[2] = '-';
  delim[3] = '-';
  memcpy(delim + 4, boundary, bnd_len);
  delim[delim_len] = '\0';

  const uint8_t *pos = memmem(body, body_len, first_delim, first_delim_len);
  if (!pos)
    return data;

  pos += first_delim_len;
  size_t remaining = body_len - (size_t)(pos - body);

  if (remaining >= 2 && pos[0] == '\r' && pos[1] == '\n') {
    pos += 2;
    remaining -= 2;
  }

  while (remaining > 0) {
    if (max_parts > 0 && data->field_count + data->file_count >= max_parts)
      break;

    const uint8_t *next = memmem(pos, remaining, delim, delim_len);
    if (!next)
      break;

    size_t part_len = (size_t)(next - pos);

    const uint8_t *header_end = memmem(pos, part_len, "\r\n\r\n", 4);
    if (!header_end) {
      pos = next + delim_len;
      remaining = body_len - (size_t)(pos - body);
      if (remaining >= 2 && pos[0] == '\r' && pos[1] == '\n') {
        pos += 2;
        remaining -= 2;
      }
      continue;
    }

    size_t headers_len = (size_t)(header_end - pos);
    const uint8_t *part_body = header_end + 4;
    size_t part_body_len = (size_t)(next - part_body);

    char *disposition = find_part_header(arena, (const char *)pos, headers_len, "Content-Disposition");
    if (!disposition) {
      pos = next + delim_len;
      remaining = body_len - (size_t)(pos - body);
      if (remaining >= 2 && pos[0] == '\r' && pos[1] == '\n') {
        pos += 2;
        remaining -= 2;
      }
      continue;
    }

    char *name = extract_param(arena, disposition, "name");
    char *filename = extract_param(arena, disposition, "filename");

    if (filename) {
      char *content_type = find_part_header(arena, (const char *)pos, headers_len, "Content-Type");
      if (!content_type)
        content_type = ecewo_strdup(arena, "application/octet-stream");

      if (data->file_count >= files_cap) {
        size_t new_cap = files_cap * 2;
        data->files = ecewo_realloc(arena, data->files,
                                    files_cap * sizeof(ecewo_multipart_file_t),
                                    new_cap * sizeof(ecewo_multipart_file_t));
        files_cap = new_cap;
      }

      ecewo_multipart_file_t *file = &data->files[data->file_count];
      file->fieldname = name ? name : ecewo_strdup(arena, "");
      file->filename = filename;
      file->mimetype = content_type;
      file->size = part_body_len;
      file->path = NULL;
      file->data = ecewo_memdup(arena, (void *)part_body, part_body_len);

      data->file_count++;
    } else if (name) {
      if (data->field_count >= fields_cap) {
        size_t new_cap = fields_cap * 2;
        data->fields = ecewo_realloc(arena, data->fields,
                                     fields_cap * sizeof(ecewo_multipart_field_t),
                                     new_cap * sizeof(ecewo_multipart_field_t));
        fields_cap = new_cap;
      }

      ecewo_multipart_field_t *field = &data->fields[data->field_count];
      field->name = name;
      char *value = ecewo_alloc(arena, part_body_len + 1);
      memcpy(value, part_body, part_body_len);
      value[part_body_len] = '\0';
      field->value = value;
      field->value_len = part_body_len;
      data->field_count++;
    }

    pos = next + delim_len;
    remaining = body_len - (size_t)(pos - body);

    if (remaining >= 2 && pos[0] == '-' && pos[1] == '-')
      break;

    if (remaining >= 2 && pos[0] == '\r' && pos[1] == '\n') {
      pos += 2;
      remaining -= 2;
    }
  }

  return data;
}

// ---------------------------------------------------------------------------
// Buffered mode: write parsed RAM files to disk asynchronously
// ---------------------------------------------------------------------------

typedef struct {
  ecewo_request_t *req;
  ecewo_response_t *res;
  ecewo_next_t next;
  _Atomic int pending;
  _Atomic bool errored;
} DiskSaveCtx;

static void on_file_saved(const char *error, void *user_data) {
  DiskSaveCtx *ctx = (DiskSaveCtx *)user_data;

  if (error) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&ctx->errored, &expected, true))
      ecewo_send_text(ctx->res, ECEWO_INTERNAL_SERVER_ERROR, "Failed to save uploaded file");
  }

  int remaining = atomic_fetch_sub(&ctx->pending, 1) - 1;
  if (remaining == 0 && !atomic_load(&ctx->errored))
    ctx->next(ctx->req, ctx->res);
}

static void on_file_renamed(const char *error, void *user_data) {
  DiskSaveCtx *ctx = (DiskSaveCtx *)user_data;

  if (error) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&ctx->errored, &expected, true))
      ecewo_send_text(ctx->res, ECEWO_INTERNAL_SERVER_ERROR, "Failed to save uploaded file");
  }

  int remaining = atomic_fetch_sub(&ctx->pending, 1) - 1;
  if (remaining == 0 && !atomic_load(&ctx->errored))
    ctx->next(ctx->req, ctx->res);
}

// Parse body, set context, optionally write files to disk, then call next.
static void multipart_finish(ecewo_request_t *req, ecewo_response_t *res, ecewo_next_t next, const uint8_t *body, size_t body_len, const char *boundary, const multipart_config_t *cfg) {
  ecewo_arena_t *arena = ecewo_req_arena(req);

  if (!body || body_len == 0) {
    ecewo_send_text(res, ECEWO_BAD_REQUEST, "Empty multipart body");
    return;
  }

  if (cfg->max_upload_size > 0 && body_len > cfg->max_upload_size) {
    ecewo_send_text(res, ECEWO_PAYLOAD_TOO_LARGE, "Upload too large");
    return;
  }

  ecewo_multipart_t *data = parse_multipart_body(arena, cfg->max_parts, body, body_len, boundary);

  ecewo_context_set(req, MULTIPART_CTX_KEY, data);

  char *local_disk_dest = cfg->disk_dest ? ecewo_strdup(arena, cfg->disk_dest) : NULL;

  if (local_disk_dest && data->file_count > 0) {
    DiskSaveCtx *ctx = ecewo_alloc(arena, sizeof(DiskSaveCtx));
    ctx->req = req;
    ctx->res = res;
    ctx->next = next;
    atomic_store(&ctx->pending, (int)data->file_count);
    atomic_store(&ctx->errored, false);

    for (size_t i = 0; i < data->file_count; i++) {
      ecewo_multipart_file_t *file = &data->files[i];

      uint64_t count = atomic_fetch_add(&g_file_counter, 1);
      time_t now = time(NULL);

      const char *ext = safe_extension(arena, file->filename);
      char *dest_path = ecewo_sprintf(arena, "%s/%lu-%lu%s", local_disk_dest,
                                      (unsigned long)now, (unsigned long)count, ext);

      if (file->path != NULL && file->data == NULL) {
        // Already on disk (spilled) — rename it
        char *old_path = file->path;
        file->path = dest_path;

        int result = fs_rename(old_path, dest_path, on_file_renamed, ctx);
        if (result != 0) {
          bool expected = false;
          if (atomic_compare_exchange_strong(&ctx->errored, &expected, true))
            ecewo_send_text(res, ECEWO_INTERNAL_SERVER_ERROR, "Failed to save uploaded file");
          return;
        }
      } else {
        file->path = dest_path;

        int result = fs_write_file(dest_path, file->data, file->size,
                                   on_file_saved, ctx);
        if (result != 0) {
          bool expected = false;
          if (atomic_compare_exchange_strong(&ctx->errored, &expected, true))
            ecewo_send_text(res, ECEWO_INTERNAL_SERVER_ERROR, "Failed to save uploaded file");
          return;
        }
      }
    }
    return; // next() called from callback
  }

  next(req, res);
}

// ---------------------------------------------------------------------------
// True streaming parser
// ---------------------------------------------------------------------------

typedef enum {
  SMP_PREAMBLE,
  SMP_HEADER,
  SMP_BODY,
  SMP_DONE,
  SMP_ERROR
} SmpState;

typedef struct SmpWriteChunk {
  char *data; // malloc'd; freed after write callback fires
  size_t len;
  struct SmpWriteChunk *next;
} SmpWriteChunk;

typedef struct SmpFile {
  char *tmp_path;
  char *dest_dir; // snapshot of cfg->disk_dest at part-start (NULL = keep tmp)
  char *fieldname;
  char *filename;
  char *mimetype;
  size_t bytes_written;
  SmpWriteChunk *queue_head;
  SmpWriteChunk *queue_tail;
  bool write_in_flight;
  bool part_done;
} SmpFile;

typedef struct StreamMpCtx StreamMpCtx;

typedef struct SmpWriteCb {
  StreamMpCtx *ctx;
  SmpFile *file;
  SmpWriteChunk *chunk; // chunk being written; freed in callback; NULL for rename
} SmpWriteCb;

struct StreamMpCtx {
  SmpState state;

  char *first_delim;
  size_t first_delim_len;
  char *body_delim;
  size_t body_delim_len;

  char *tail;
  size_t tail_len;
  size_t tail_cap;

  char *header_buf;
  size_t header_len;
  size_t header_cap;

  bool cur_is_file;
  char *cur_fieldname;
  char *cur_filename;
  char *cur_mimetype;
  SmpFile *cur_file;

  char *field_buf;
  size_t field_len;
  size_t field_buf_cap;

  ecewo_multipart_field_t *fields;
  size_t field_count;
  size_t fields_cap;

  ecewo_multipart_file_t *files;
  size_t file_count;
  size_t files_cap;

  size_t total_bytes;
  size_t parts_count;

  _Atomic int pending_writes;
  bool cancelled;
  bool body_ended;

  size_t max_upload_size;
  size_t max_parts;

  ecewo_request_t *req;
  ecewo_response_t *res;
  ecewo_next_t next;
};

static void smp_flush_write_queue(StreamMpCtx *ctx, SmpFile *file);
static void smp_finalize_file(StreamMpCtx *ctx, SmpFile *file);
static void smp_try_finish(StreamMpCtx *ctx);

static void smp_on_rename_done(const char *error, void *user_data) {
  SmpWriteCb *cb = (SmpWriteCb *)user_data;
  StreamMpCtx *ctx = cb->ctx;

  if (error && !ctx->cancelled) {
    ctx->cancelled = true;
    ecewo_send_text(ctx->res, ECEWO_INTERNAL_SERVER_ERROR, "Failed to save uploaded file");
  }

  atomic_fetch_sub(&ctx->pending_writes, 1);
  smp_try_finish(ctx);
}

static void smp_on_chunk_written(const char *error, void *user_data) {
  SmpWriteCb *cb = (SmpWriteCb *)user_data;
  StreamMpCtx *ctx = cb->ctx;
  SmpFile *file = cb->file;
  SmpWriteChunk *chunk = cb->chunk;

  free(chunk->data);
  free(chunk);
  file->write_in_flight = false;

  if (error && !ctx->cancelled) {
    ctx->cancelled = true;
    ecewo_send_text(ctx->res, ECEWO_INTERNAL_SERVER_ERROR, "Failed to write uploaded file");
  }

  if (ctx->cancelled) {
    SmpWriteChunk *c = file->queue_head;
    while (c) {
      SmpWriteChunk *n = c->next;
      free(c->data);
      free(c);
      c = n;
    }
    file->queue_head = file->queue_tail = NULL;
    if (file->part_done) {
      atomic_fetch_sub(&ctx->pending_writes, 1);
    }
    return;
  }

  if (file->queue_head) {
    smp_flush_write_queue(ctx, file);
  } else if (file->part_done) {
    smp_finalize_file(ctx, file);
  }
}

static void smp_flush_write_queue(StreamMpCtx *ctx, SmpFile *file) {
  if (file->write_in_flight || !file->queue_head)
    return;

  SmpWriteChunk *chunk = file->queue_head;
  file->queue_head = chunk->next;
  if (!file->queue_head)
    file->queue_tail = NULL;

  file->write_in_flight = true;

  SmpWriteCb *cb = ecewo_alloc(ecewo_req_arena(ctx->req), sizeof(SmpWriteCb));
  cb->ctx = ctx;
  cb->file = file;
  cb->chunk = chunk;

  if (fs_append_file(file->tmp_path, chunk->data, chunk->len,
                     smp_on_chunk_written, cb)
      != 0) {
    file->write_in_flight = false;
    free(chunk->data);
    free(chunk);
    if (!ctx->cancelled) {
      ctx->cancelled = true;
      ecewo_send_text(ctx->res, ECEWO_INTERNAL_SERVER_ERROR, "Failed to write uploaded file");
    }
    if (file->part_done)
      atomic_fetch_sub(&ctx->pending_writes, 1);
  }
}

static void smp_enqueue_chunk(StreamMpCtx *ctx, SmpFile *file, const char *data, size_t len) {
  if (len == 0 || ctx->cancelled)
    return;

  char *copy = malloc(len);
  SmpWriteChunk *chunk = malloc(sizeof(SmpWriteChunk));
  if (!copy || !chunk) {
    free(copy);
    free(chunk);
    if (!ctx->cancelled) {
      ctx->cancelled = true;
      ecewo_send_text(ctx->res, ECEWO_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    return;
  }
  memcpy(copy, data, len);
  chunk->data = copy;
  chunk->len = len;
  chunk->next = NULL;

  if (file->queue_tail) {
    file->queue_tail->next = chunk;
    file->queue_tail = chunk;
  } else {
    file->queue_head = file->queue_tail = chunk;
  }
  file->bytes_written += len;
  smp_flush_write_queue(ctx, file);
}

static void smp_finalize_file(StreamMpCtx *ctx, SmpFile *file) {
  ecewo_arena_t *arena = ecewo_req_arena(ctx->req);

  if (ctx->file_count >= ctx->files_cap) {
    size_t new_cap = ctx->files_cap ? ctx->files_cap * 2 : 4;
    ctx->files = ecewo_realloc(arena, ctx->files,
                               ctx->files_cap * sizeof(ecewo_multipart_file_t),
                               new_cap * sizeof(ecewo_multipart_file_t));
    ctx->files_cap = new_cap;
  }

  ecewo_multipart_file_t *mf = &ctx->files[ctx->file_count++];
  mf->fieldname = file->fieldname;
  mf->filename = file->filename;
  mf->mimetype = file->mimetype;
  mf->data = NULL;
  mf->size = file->bytes_written;

  if (file->bytes_written == 0) {
    mf->path = NULL;
    atomic_fetch_sub(&ctx->pending_writes, 1);
    smp_try_finish(ctx);
    return;
  }

  if (file->dest_dir) {
    uint64_t count = atomic_fetch_add(&g_file_counter, 1);
    time_t now = time(NULL);
    const char *ext = safe_extension(arena, file->filename);
    char *dest_path = ecewo_sprintf(arena, "%s/%lu-%lu%s", file->dest_dir,
                                    (unsigned long)now, (unsigned long)count, ext);
    mf->path = dest_path;

    SmpWriteCb *cb = ecewo_alloc(arena, sizeof(SmpWriteCb));
    cb->ctx = ctx;
    cb->file = file;
    cb->chunk = NULL;

    if (fs_rename(file->tmp_path, dest_path, smp_on_rename_done, cb) != 0) {
      if (!ctx->cancelled) {
        ctx->cancelled = true;
        ecewo_send_text(ctx->res, ECEWO_INTERNAL_SERVER_ERROR, "Failed to save uploaded file");
      }
      atomic_fetch_sub(&ctx->pending_writes, 1);
      smp_try_finish(ctx);
    }
  } else {
    mf->path = file->tmp_path;
    atomic_fetch_sub(&ctx->pending_writes, 1);
    smp_try_finish(ctx);
  }
}

static void smp_try_finish(StreamMpCtx *ctx) {
  if (ctx->cancelled)
    return;
  if (!ctx->body_ended)
    return;
  if (atomic_load(&ctx->pending_writes) > 0)
    return;

  ecewo_multipart_t *data = ecewo_alloc(ecewo_req_arena(ctx->req), sizeof(ecewo_multipart_t));
  data->fields = ctx->fields;
  data->field_count = ctx->field_count;
  data->files = ctx->files;
  data->file_count = ctx->file_count;

  ecewo_context_set(ctx->req, MULTIPART_CTX_KEY, data);
  ctx->next(ctx->req, ctx->res);
}

static void smp_header_append(StreamMpCtx *ctx, const char *data, size_t len) {
  ecewo_arena_t *arena = ecewo_req_arena(ctx->req);
  if (ctx->header_len + len + 1 > ctx->header_cap) {
    size_t new_cap = ctx->header_cap ? ctx->header_cap * 2 : 512;
    while (new_cap < ctx->header_len + len + 1)
      new_cap *= 2;
    ctx->header_buf = ecewo_realloc(arena, ctx->header_buf, ctx->header_cap, new_cap);
    ctx->header_cap = new_cap;
  }
  memcpy(ctx->header_buf + ctx->header_len, data, len);
  ctx->header_len += len;
}

static void smp_parse_headers(StreamMpCtx *ctx, const char *disk_dest) {
  ctx->header_buf[ctx->header_len] = '\0';
  ecewo_arena_t *arena = ecewo_req_arena(ctx->req);

  char *disposition = find_part_header(arena, ctx->header_buf,
                                       ctx->header_len, "Content-Disposition");
  if (!disposition) {
    ctx->cur_fieldname = NULL;
    ctx->cur_filename = NULL;
    ctx->cur_mimetype = NULL;
    ctx->cur_is_file = false;
    ctx->cur_file = NULL;
    ctx->header_len = 0;
    return;
  }

  ctx->cur_fieldname = extract_param(arena, disposition, "name");
  ctx->cur_filename = extract_param(arena, disposition, "filename");
  ctx->cur_is_file = (ctx->cur_filename != NULL);

  if (ctx->cur_is_file) {
    char *ct = find_part_header(arena, ctx->header_buf, ctx->header_len,
                                "Content-Type");
    ctx->cur_mimetype = ct ? ct : ecewo_strdup(arena, "application/octet-stream");

    SmpFile *f = ecewo_alloc(arena, sizeof(SmpFile));
    memset(f, 0, sizeof(SmpFile));
    f->fieldname = ctx->cur_fieldname ? ctx->cur_fieldname : ecewo_strdup(arena, "");
    f->filename = ctx->cur_filename;
    f->mimetype = ctx->cur_mimetype;

    // Stage in the destination directory to keep the final rename intra-FS (no EXDEV).
    f->dest_dir = disk_dest ? ecewo_strdup(arena, disk_dest) : NULL;

    if (f->dest_dir) {
      uint64_t cnt = atomic_fetch_add(&g_file_counter, 1);
      f->tmp_path = ecewo_sprintf(arena, "%s/ecewo-mp-%lu-%lu-%d.tmp", f->dest_dir,
                                  (unsigned long)time(NULL), (unsigned long)cnt,
                                  (int)getpid());
    } else {
      f->tmp_path = generate_tmp_path(arena);
    }

    ctx->cur_file = f;

    atomic_fetch_add(&ctx->pending_writes, 1);
  } else {
    ctx->cur_file = NULL;
    ctx->field_len = 0;
  }

  ctx->header_len = 0;
}

static void smp_flush_body(StreamMpCtx *ctx, const char *data, size_t len) {
  if (len == 0)
    return;
  if (ctx->cur_is_file) {
    if (ctx->cur_file)
      smp_enqueue_chunk(ctx, ctx->cur_file, data, len);
  } else if (ctx->cur_fieldname) {
    ecewo_arena_t *arena = ecewo_req_arena(ctx->req);
    if (ctx->field_len + len + 1 > ctx->field_buf_cap) {
      size_t new_cap = ctx->field_buf_cap ? ctx->field_buf_cap * 2 : 256;
      while (new_cap < ctx->field_len + len + 1)
        new_cap *= 2;
      ctx->field_buf = ecewo_realloc(arena, ctx->field_buf,
                                     ctx->field_buf_cap, new_cap);
      ctx->field_buf_cap = new_cap;
    }
    memcpy(ctx->field_buf + ctx->field_len, data, len);
    ctx->field_len += len;
  }
}

static void smp_end_part(StreamMpCtx *ctx) {
  ecewo_arena_t *arena = ecewo_req_arena(ctx->req);

  if (ctx->cur_is_file) {
    if (ctx->cur_file) {
      SmpFile *f = ctx->cur_file;
      f->part_done = true;
      if (!f->write_in_flight && !f->queue_head)
        smp_finalize_file(ctx, f);
    }
  } else if (ctx->cur_fieldname) {
    if (ctx->field_count >= ctx->fields_cap) {
      size_t new_cap = ctx->fields_cap ? ctx->fields_cap * 2 : 4;
      ctx->fields = ecewo_realloc(arena, ctx->fields,
                                  ctx->fields_cap * sizeof(ecewo_multipart_field_t),
                                  new_cap * sizeof(ecewo_multipart_field_t));
      ctx->fields_cap = new_cap;
    }
    char *val;
    size_t val_len = ctx->field_len;
    if (ctx->field_buf && ctx->field_len > 0) {
      ctx->field_buf[ctx->field_len] = '\0';
      val = ctx->field_buf;
    } else {
      val = ecewo_strdup(arena, "");
      val_len = 0;
    }
    ecewo_multipart_field_t *mf = &ctx->fields[ctx->field_count++];
    mf->name = ctx->cur_fieldname;
    mf->value = val;
    mf->value_len = val_len;
  }

  ctx->cur_fieldname = NULL;
  ctx->cur_filename = NULL;
  ctx->cur_mimetype = NULL;
  ctx->cur_is_file = false;
  ctx->cur_file = NULL;
  ctx->field_buf = NULL;
  ctx->field_len = 0;
  ctx->field_buf_cap = 0;
}

static void smp_process_data(StreamMpCtx *ctx, const char *disk_dest, const char *data, size_t len) {
  size_t pos = 0;

  while (pos < len && ctx->state != SMP_DONE && ctx->state != SMP_ERROR && !ctx->cancelled) {

    switch (ctx->state) {
    case SMP_PREAMBLE: {
      const char *found = memmem(data + pos, len - pos, ctx->first_delim,
                                 ctx->first_delim_len);
      if (!found) {
        size_t window = len - pos;
        size_t hold = ctx->first_delim_len - 1 < window
            ? ctx->first_delim_len - 1
            : window;
        ctx->tail_len = hold;
        if (hold)
          memcpy(ctx->tail, data + len - hold, hold);
        return;
      }
      size_t fpos = (size_t)(found - data);
      size_t after = fpos + ctx->first_delim_len;
      if (after + 2 > len) {
        ctx->tail_len = len - fpos;
        memcpy(ctx->tail, data + fpos, ctx->tail_len);
        return;
      }
      if (data[after] == '\r' && data[after + 1] == '\n') {
        ctx->state = SMP_HEADER;
        pos = after + 2;
      } else if (data[after] == '-' && data[after + 1] == '-') {
        ctx->state = SMP_DONE;
        return;
      } else {
        pos = after;
      }
      break;
    }

    case SMP_HEADER: {
      const char *found = memmem(data + pos, len - pos, "\r\n\r\n", 4);
      if (!found) {
        size_t available = len - pos;
        size_t hold = available < 3 ? available : 3;
        size_t add = available - hold;
        if (add)
          smp_header_append(ctx, data + pos, add);
        ctx->tail_len = hold;
        if (hold)
          memcpy(ctx->tail, data + pos + add, hold);
        return;
      }
      size_t fpos = (size_t)(found - data);
      if (fpos > pos)
        smp_header_append(ctx, data + pos, fpos - pos);
      smp_parse_headers(ctx, disk_dest);

      ctx->parts_count++;
      if (ctx->max_parts > 0 && ctx->parts_count > ctx->max_parts) {
        ctx->state = SMP_DONE;
        return;
      }
      ctx->state = SMP_BODY;
      pos = fpos + 4;
      break;
    }

    case SMP_BODY: {
      const char *found = memmem(data + pos, len - pos, ctx->body_delim,
                                 ctx->body_delim_len);
      if (!found) {
        size_t available = len - pos;
        size_t hold = ctx->body_delim_len - 1 < available
            ? ctx->body_delim_len - 1
            : available;
        size_t flush_len = available - hold;
        if (flush_len)
          smp_flush_body(ctx, data + pos, flush_len);
        ctx->tail_len = hold;
        if (hold)
          memcpy(ctx->tail, data + pos + flush_len, hold);
        return;
      }
      size_t fpos = (size_t)(found - data);
      if (fpos > pos)
        smp_flush_body(ctx, data + pos, fpos - pos);

      size_t after = fpos + ctx->body_delim_len;
      if (after + 2 > len) {
        ctx->tail_len = len - fpos;
        memcpy(ctx->tail, data + fpos, ctx->tail_len);
        return;
      }

      smp_end_part(ctx);

      if (data[after] == '\r' && data[after + 1] == '\n') {
        ctx->state = SMP_HEADER;
        pos = after + 2;
      } else {
        ctx->state = SMP_DONE;
        return;
      }
      break;
    }

    default:
      return;
    }
  }
}

static void smp_on_chunk(ecewo_request_t *req, const uint8_t *data, size_t len) {
  StreamMpCtx *ctx = ecewo_context_get(req, STREAM_MP_KEY);
  if (!ctx || ctx->cancelled)
    return;

  ctx->total_bytes += len;
  if (ctx->max_upload_size > 0 && ctx->total_bytes > ctx->max_upload_size) {
    ctx->cancelled = true;
    ecewo_send_text(ctx->res, ECEWO_PAYLOAD_TOO_LARGE, "Upload too large");
    return;
  }

  // Combine tail + new chunk into a temporary work buffer.
  // malloc, not arena, so this scratch space doesn't accumulate across the upload.
  size_t work_len = ctx->tail_len + len;
  char *work = malloc(work_len);
  if (!work) {
    ctx->cancelled = true;
    ecewo_send_text(ctx->res, ECEWO_INTERNAL_SERVER_ERROR, "Out of memory");
    return;
  }
  if (ctx->tail_len)
    memcpy(work, ctx->tail, ctx->tail_len);
  memcpy(work + ctx->tail_len, data, len);
  ctx->tail_len = 0;

  const multipart_config_t *cfg = cfg_for_request(req);
  const char *disk_dest = cfg ? cfg->disk_dest : NULL;

  smp_process_data(ctx, disk_dest, work, work_len);
  free(work);
}

static void smp_on_end(ecewo_request_t *req, ecewo_response_t *res) {
  (void)res;
  StreamMpCtx *ctx = ecewo_context_get(req, STREAM_MP_KEY);
  if (!ctx || ctx->cancelled)
    return;

  const multipart_config_t *cfg = cfg_for_request(req);
  const char *disk_dest = cfg ? cfg->disk_dest : NULL;

  if (ctx->tail_len > 0) {
    char tail_copy[512];
    size_t tail_len = ctx->tail_len;
    memcpy(tail_copy, ctx->tail, tail_len);
    ctx->tail_len = 0;
    smp_process_data(ctx, disk_dest, tail_copy, tail_len);
  }

  if (ctx->state == SMP_BODY)
    smp_end_part(ctx);
  if (ctx->state != SMP_DONE)
    ctx->state = SMP_DONE;

  ctx->body_ended = true;
  smp_try_finish(ctx);
}

// ---------------------------------------------------------------------------
// Middleware entry point
// ---------------------------------------------------------------------------

void ecewo_multipart(ecewo_request_t *req, ecewo_response_t *res, ecewo_next_t next) {
  const char *content_type = ecewo_header_get(req, "Content-Type");

  if (!content_type) {
    next(req, res);
    return;
  }

  ecewo_arena_t *arena = ecewo_req_arena(req);
  char *boundary = extract_boundary(arena, content_type);
  if (!boundary) {
    next(req, res);
    return;
  }

  // Lazy default-initialize config so handlers that didn't call any setter still work.
  multipart_config_t *cfg = cfg_get_or_create(ecewo_req_app(req));
  if (!cfg) {
    ecewo_send_text(res, ECEWO_INTERNAL_SERVER_ERROR, "multipart config unavailable");
    return;
  }

  const uint8_t *body = ecewo_req_body(req);
  size_t body_len = ecewo_req_body_len(req);

  // Streaming mode: ecewo_body_stream ran before us
  if (body == NULL && body_len == 0 && ecewo_context_get(req, "_body_stream") != NULL) {
    size_t bnd_len = strlen(boundary);

    StreamMpCtx *ctx = ecewo_alloc(arena, sizeof(StreamMpCtx));
    memset(ctx, 0, sizeof(StreamMpCtx));

    ctx->first_delim = ecewo_alloc(arena, bnd_len + 3);
    ctx->first_delim[0] = '-';
    ctx->first_delim[1] = '-';
    memcpy(ctx->first_delim + 2, boundary, bnd_len);
    ctx->first_delim[bnd_len + 2] = '\0';
    ctx->first_delim_len = bnd_len + 2;

    ctx->body_delim = ecewo_alloc(arena, bnd_len + 5);
    ctx->body_delim[0] = '\r';
    ctx->body_delim[1] = '\n';
    ctx->body_delim[2] = '-';
    ctx->body_delim[3] = '-';
    memcpy(ctx->body_delim + 4, boundary, bnd_len);
    ctx->body_delim[bnd_len + 4] = '\0';
    ctx->body_delim_len = bnd_len + 4;

    ctx->tail_cap = ctx->body_delim_len + 2;
    ctx->tail = ecewo_alloc(arena, ctx->tail_cap);
    ctx->state = SMP_PREAMBLE;
    atomic_store(&ctx->pending_writes, 0);
    ctx->req = req;
    ctx->res = res;
    ctx->next = next;
    ctx->max_upload_size = cfg->max_upload_size;
    ctx->max_parts = cfg->max_parts;

    ecewo_context_set(req, STREAM_MP_KEY, ctx);
    ecewo_body_on_data(req, smp_on_chunk);
    ecewo_body_on_end(req, res, smp_on_end);
    return;
  }

  multipart_finish(req, res, next, body, body_len, boundary, cfg);
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------

ecewo_multipart_t *ecewo_multipart_get(const ecewo_request_t *req) {
  return (ecewo_multipart_t *)ecewo_context_get((ecewo_request_t *)req, MULTIPART_CTX_KEY);
}

size_t ecewo_multipart_field_count(const ecewo_multipart_t *mp) {
  return mp ? mp->field_count : 0;
}

size_t ecewo_multipart_file_count(const ecewo_multipart_t *mp) {
  return mp ? mp->file_count : 0;
}

const ecewo_multipart_field_t *ecewo_multipart_field_at(const ecewo_multipart_t *mp, size_t i) {
  if (!mp || i >= mp->field_count)
    return NULL;
  return &mp->fields[i];
}

const ecewo_multipart_file_t *ecewo_multipart_file_at(const ecewo_multipart_t *mp, size_t i) {
  if (!mp || i >= mp->file_count)
    return NULL;
  return &mp->files[i];
}

const ecewo_multipart_field_t *ecewo_multipart_get_field(const ecewo_request_t *req, const char *name) {
  ecewo_multipart_t *mp = ecewo_multipart_get(req);
  if (!mp || !name)
    return NULL;
  for (size_t i = 0; i < mp->field_count; i++) {
    if (strcmp(mp->fields[i].name, name) == 0)
      return &mp->fields[i];
  }
  return NULL;
}

const char *ecewo_multipart_get_field_value(const ecewo_request_t *req, const char *name) {
  const ecewo_multipart_field_t *f = ecewo_multipart_get_field(req, name);
  return f ? f->value : NULL;
}

const ecewo_multipart_file_t *ecewo_multipart_get_file(const ecewo_request_t *req, const char *fieldname) {
  ecewo_multipart_t *mp = ecewo_multipart_get(req);
  if (!mp || !fieldname)
    return NULL;
  for (size_t i = 0; i < mp->file_count; i++) {
    if (strcmp(mp->files[i].fieldname, fieldname) == 0)
      return &mp->files[i];
  }
  return NULL;
}

const char *ecewo_multipart_field_name(const ecewo_multipart_field_t *f) { return f ? f->name : NULL; }
const char *ecewo_multipart_field_value(const ecewo_multipart_field_t *f) { return f ? f->value : NULL; }
size_t ecewo_multipart_field_value_len(const ecewo_multipart_field_t *f) { return f ? f->value_len : 0; }

const char *ecewo_multipart_file_fieldname(const ecewo_multipart_file_t *f) { return f ? f->fieldname : NULL; }
const char *ecewo_multipart_file_filename(const ecewo_multipart_file_t *f) { return f ? f->filename : NULL; }
const char *ecewo_multipart_file_mimetype(const ecewo_multipart_file_t *f) { return f ? f->mimetype : NULL; }
const uint8_t *ecewo_multipart_file_data(const ecewo_multipart_file_t *f) { return f ? f->data : NULL; }
size_t ecewo_multipart_file_size(const ecewo_multipart_file_t *f) { return f ? f->size : 0; }
const char *ecewo_multipart_file_path(const ecewo_multipart_file_t *f) { return f ? f->path : NULL; }
