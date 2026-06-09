#ifndef LSP_H
#define LSP_H
#include "buffer.h"
#include "json.h"
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_oldnames.h>
#include <pthread.h>
#include <sys/types.h>

typedef struct {
  int in_fd;
  int out_fd;
  pid_t pid;
  pthread_t render_thread;
  // thread-safe queue
  JsonValue **msg_queue;
  int queue_head, queue_tial, queue_cap;
  SDL_Mutex *queue_mutex;
  int running;
  int request_id;
  char *file_path;
  char *file_uri;
  JsonValue *pending_result;
  int pending_id;
} LspClient;

LspClient *lsp_init(const char *file_path);
void lsp_open(LspClient *lc, Buffer *buf);
void lsp_change(LspClient *lc, Buffer *buf);
void lsp_request_diagnostics(LspClient *lc);
JsonValue *lsp_poll_message(LspClient *lc);
void lsp_destroy(LspClient *lc);
void lsp_request_completion(LspClient *lc, int line, int col);
JsonValue *lsp_get_pending_result(LspClient *lc);
void lsp_request_hover(LspClient *lc, int line, int col);
void lsp_request_definition(LspClient *lc, int line, int col);
void lsp_close(LspClient *lc);

#endif
