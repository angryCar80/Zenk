#include "lsp.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void build_uri(const char *path, char *out, int out_size) {
  snprintf(out, out_size, "file://%s", path);
}

static int write_all(int fd, const char *data, int len) {
  int written = 0;
  while (written < len) {
    int n = write(fd, data + written, len - written);
    if (n <= 0)
      return -1;
    written += n;
  }
  return written;
}

static void send_data(LspClient *lc, const char *body) {
  char header[64];
  int len = strlen(body);
  snprintf(header, sizeof(header), "Content-Length: %d\r\n\r\n", len);
  write_all(lc->in_fd, header, strlen(header));
  write_all(lc->in_fd, body, len);
}

static char *escape_json(const char *s) {
  int cap = strlen(s) * 2 + 2;
  char *out = malloc(cap);
  int j = 0;
  for (int i = 0; s[i]; i++) {
    if (j >= cap - 6) {
      cap *= 2;
      out = realloc(out, cap);
    }
    switch (s[i]) {
    case '"':
      out[j++] = '\\';
      out[j++] = '"';
      break;
    case '\\':
      out[j++] = '\\';
      out[j++] = '\\';
      break;
    case '\n':
      out[j++] = '\\';
      out[j++] = 'n';
      break;
    case '\t':
      out[j++] = '\\';
      out[j++] = 't';
      break;
    case '\r':
      out[j++] = '\\';
      out[j++] = 'r';
      break;
    default:
      out[j++] = s[i];
      break;
    }
  }
  out[j] = '\0';
  return out;
}

static char *join_lines(Buffer *buf) {
  int total = 1;
  for (int i = 0; i < buf->line_count; i++)
    total += strlen(buf->lines[i]) + 1;
  char *out = malloc(total);
  int pos = 0;
  for (int i = 0; i < buf->line_count; i++) {
    int len = strlen(buf->lines[i]);
    memcpy(out + pos, buf->lines[i], len);
    pos += len;
    if (i < buf->line_count - 1)
      out[pos++] = '\n';
  }
  out[pos] = '\0';
  return out;
}

static void enqueue(LspClient *lc, JsonValue *msg) {
  SDL_LockMutex(lc->queue_mutex);
  int next = (lc->queue_tial + 1) % lc->queue_cap;
  if (next == lc->queue_head) {
    int new_cap = lc->queue_cap * 2;
    JsonValue **new_q = malloc(sizeof(JsonValue *) * new_cap);
    int idx = 0;
    for (int i = lc->queue_head; i != lc->queue_tial;
         i = (i + 1) % lc->queue_cap)
      new_q[idx++] = lc->msg_queue[i];
    lc->queue_head = 0;
    lc->queue_tial = idx;
    lc->queue_cap = new_cap;
    free(lc->msg_queue);
    lc->msg_queue = new_q;
    next = (lc->queue_tial + 1) % lc->queue_cap;
  }
  lc->msg_queue[lc->queue_tial] = msg;
  lc->queue_tial = next;
  SDL_UnlockMutex(lc->queue_mutex);
}

static void *reader_thread(void *arg) {
  LspClient *lc = arg;
  char buf[65536];
  int buf_len = 0;

  while (lc->running) {
    int n = read(lc->out_fd, buf + buf_len, sizeof(buf) - buf_len - 1);
    if (n <= 0)
      break;
    buf_len += n;
    buf[buf_len] = '\0';

    while (1) {
      char *end = strstr(buf, "\r\n\r\n");
      if (!end)
        break;

      int header_len = (end + 4) - buf;
      int content_len = 0;
      char *cl = strstr(buf, "Content-Length:");
      if (cl)
        sscanf(cl, "Content-Length: %d", &content_len);

      int total = header_len + content_len;
      if (buf_len < total)
        break;

      buf[header_len + content_len] = '\0';
      JsonValue *msg = json_parse(buf + header_len);
      if (msg) {
        JsonValue *id_val = json_get(msg, "id");
        if (id_val && id_val->type == JSON_NUM &&
            (int)id_val->number == lc->pending_id) {
          SDL_LockMutex(lc->queue_mutex);
          if (lc->pending_result) json_free(lc->pending_result);
          lc->pending_result = msg;
          SDL_UnlockMutex(lc->queue_mutex);
        } else {
          enqueue(lc, msg);
        }
      }

      int remaining = buf_len - total;
      memmove(buf, buf + total, remaining);
      buf_len = remaining;
    }
  }
  return NULL;
}

LspClient *lsp_init(const char *file_path) {
  LspClient *lc = calloc(1, sizeof(LspClient));

  // Resolve to absolute path for correct file:// URI
  char abs_path[512];
  if (!realpath(file_path, abs_path))
    strncpy(abs_path, file_path, sizeof(abs_path));
  lc->file_path = strdup(abs_path);

  char uri[512];
  build_uri(abs_path, uri, sizeof(uri));
  lc->file_uri = strdup(uri);

  int to_child[2], from_child[2];
  if (pipe(to_child) < 0 || pipe(from_child) < 0) {
    free(lc->file_path);
    free(lc->file_uri);
    free(lc);
    return NULL;
  }

  lc->pid = fork();
  if (lc->pid == 0) {
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    execlp("clangd", "clangd", NULL);
    _exit(1);
  }

  close(to_child[0]);
  close(from_child[1]);
  lc->in_fd = to_child[1];
  lc->out_fd = from_child[0];

  lc->msg_queue = malloc(sizeof(JsonValue *) * 64);
  lc->queue_head = 0;
  lc->queue_tial = 0;
  lc->queue_cap = 64;
  lc->queue_mutex = SDL_CreateMutex();
  lc->request_id = 1;
  lc->running = 1;

  // Start reader thread first — it enqueues everything
  pthread_create(&lc->render_thread, NULL, reader_thread, lc);

  // Send initialize request
  char init_body[1024];
  snprintf(init_body, sizeof(init_body),
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
    "{\"processId\":null,\"capabilities\":{},\"rootUri\":null}}");
  send_data(lc, init_body);

  // Wait for the initialize response from the queue (spin with 5ms sleeps)
  // Other messages (progress) will be discarded
  for (int tries = 0; tries < 200; tries++) {
    usleep(5000);
    JsonValue *msg = lsp_poll_message(lc);
    while (msg) {
      JsonValue *id_val = json_get(msg, "id");
      if (id_val && id_val->type == JSON_NUM && (int)id_val->number == 1) {
        json_free(msg);
        goto init_ok;
      }
      json_free(msg);
      msg = lsp_poll_message(lc);
    }
  }
  // Timeout — clangd didn't respond
  lsp_destroy(lc);
  return NULL;

init_ok:
  // Send initialized notification
  send_data(lc, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

  lc->request_id = 2;
  return lc;
}

void lsp_open(LspClient *lc, Buffer *buf) {
  char *content = join_lines(buf);
  char *escaped = escape_json(content);
  free(content);

  char body[65536];
  snprintf(
      body, sizeof(body),
      "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
      "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":1,"
      "\"text\":\"%s\"}}}",
      lc->file_uri, escaped);
  free(escaped);

  send_data(lc, body);
}

void lsp_change(LspClient *lc, Buffer *buf) {
  char *content = join_lines(buf);
  char *escaped = escape_json(content);
  free(content);

  lc->request_id++;
  char body[65536];
  snprintf(
      body, sizeof(body),
      "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":"
      "{\"textDocument\":{\"uri\":\"%s\",\"version\":%d},"
      "\"contentChanges\":[{\"text\":\"%s\"}]}}",
      lc->file_uri, lc->request_id, escaped);
  free(escaped);

  send_data(lc, body);
}

void lsp_request_diagnostics(LspClient *lc) {
  lc->request_id++;
  char body[4096];
  snprintf(body, sizeof(body),
           "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"textDocument/"
           "diagnostic\",\"params\":"
           "{\"textDocument\":{\"uri\":\"%s\"}}}",
           lc->request_id, lc->file_uri);
  send_data(lc, body);
}

JsonValue *lsp_poll_message(LspClient *lc) {
  SDL_LockMutex(lc->queue_mutex);
  JsonValue *msg = NULL;
  if (lc->queue_head != lc->queue_tial) {
    msg = lc->msg_queue[lc->queue_head];
    lc->queue_head = (lc->queue_head + 1) % lc->queue_cap;
  }
  SDL_UnlockMutex(lc->queue_mutex);
  return msg;
}

void lsp_destroy(LspClient *lc) {
  close(lc->in_fd);
  close(lc->out_fd);
  lc->running = 0;
  pthread_join(lc->render_thread, NULL);
  free(lc->file_path);
  free(lc->file_uri);
  SDL_DestroyMutex(lc->queue_mutex);
  free(lc->msg_queue);
  free(lc);
}

JsonValue *lsp_get_pending_result(LspClient *lc) {
  SDL_LockMutex(lc->queue_mutex);
  JsonValue *r = lc->pending_result;
  lc->pending_result = NULL;
  SDL_UnlockMutex(lc->queue_mutex);
  return r;
}

void lsp_request_completion(LspClient *lc, int line, int col) {
  lc->request_id++;
  lc->pending_id = lc->request_id;

  if (lc->pending_result) {
    json_free(lc->pending_result);
    lc->pending_result = NULL;
  }
  char body[4096];
  snprintf(body, sizeof(body),
           "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"textDocument/"
           "completion\",\"params\":"
           "{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%d,"
           "\"character\":%d}}}",
           lc->request_id, lc->file_uri, line, col);
  send_data(lc, body);
}
