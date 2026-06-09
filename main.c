#include "buffer.h"
#include "json.h"
#include "lsp.h"
#include "user.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_oldnames.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define UNDO_MAX 100

typedef struct {
  int line;
  int severity;
  char message[256];
} Diag;

Diag diagnostics[256];
int diag_count = 0;

typedef struct { char label[256]; char insert[256]; char detail[256]; } CompletionItem;
CompletionItem completions[128];
int completion_count = 0;
CompletionItem base_completions[128];
int base_count = 0;
int completion_selected = 0;

int CHAR_WIDTH = 8;
int CHAR_HEIGHT = 16;
int GUTTER_WIDTH;

typedef struct {
  float x;
  float y;
} Cursor;

// UndoEntry struct (before main)
typedef struct {
  Buffer buf;
  int row, col;
} UndoEntry;

// Stacks (inside main, next to swallow_text / pending_operator)
UndoEntry undo_stack[UNDO_MAX];
UndoEntry redo_stack[UNDO_MAX];
int undo_count = 0, redo_count = 0;

// helper (also inside main, before the game loop)
void push_undo(Buffer *buf, int row, int col) {
  if (undo_count < UNDO_MAX) {
    undo_stack[undo_count].buf = buffer_clone(buf);
    undo_stack[undo_count].row = row;
    undo_stack[undo_count].col = col;
    undo_count++;
  }
  redo_count = 0; // new action clears redo
}

void undo(Buffer *buf, int *row, int *col) {
  if (undo_count == 0)
    return;
  UndoEntry e = undo_stack[--undo_count];
  // push current to redo
  if (redo_count < UNDO_MAX) {
    redo_stack[redo_count].buf = buffer_clone(buf);
    redo_stack[redo_count].row = *row;
    redo_stack[redo_count].col = *col;
    redo_count++;
  }
  // restore
  buffer_destroy(buf);
  *buf = e.buf;
  *row = e.row;
  *col = e.col;
}

void redo(Buffer *buf, int *row, int *col) {
  if (redo_count == 0) {
    return;
  }
  UndoEntry e = redo_stack[--redo_count];

  if (undo_count < UNDO_MAX) {
    undo_stack[undo_count].buf = buffer_clone(buf);
    undo_stack[undo_count].row = *row;
    undo_stack[undo_count].col = *col;
    undo_count++;
  }
  buffer_destroy(buf);
  *buf = e.buf;
  *row = e.row;
  *col = e.col;
}

void DrawChar(SDL_Renderer *renderer, SDL_Texture *fontTexture, char c, int x,
              int y) {
  int ascii_offset = c - ' ';

  SDL_FRect srcRect;
  SDL_FRect dstRect;
  srcRect.x = ascii_offset * CHAR_WIDTH;
  srcRect.y = 0;
  srcRect.w = CHAR_WIDTH;
  srcRect.h = CHAR_HEIGHT;

  dstRect.x = x;
  dstRect.y = y;
  dstRect.w = CHAR_WIDTH;
  dstRect.h = CHAR_HEIGHT;

  SDL_RenderTexture(renderer, fontTexture, &srcRect, &dstRect);
}

int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return a > b ? a : b; }

int search_forward(Buffer *buf, char *pattern, int start_row, int start_col,
                   int *out_row, int *out_col) {
  // Search from start position forward, wrapping around
  int len = strlen(pattern);
  if (len == 0) return 0;
  for (int r = start_row; r < buf->line_count; r++) {
    int s = (r == start_row) ? start_col : 0;
    char *p = strstr(buf->lines[r] + s, pattern);
    if (p) {
      *out_row = r;
      *out_col = (int)(p - buf->lines[r]);
      return 1;
    }
  }
  // wrap to top
  for (int r = 0; r < start_row; r++) {
    char *p = strstr(buf->lines[r], pattern);
    if (p) {
      *out_row = r;
      *out_col = (int)(p - buf->lines[r]);
      return 1;
    }
  }
  return 0;
}

int search_backward(Buffer *buf, char *pattern, int start_row, int start_col,
                    int *out_row, int *out_col) {
  int len = strlen(pattern);
  if (len == 0) return 0;
  for (int r = start_row; r >= 0; r--) {
    int end = (r == start_row) ? start_col : (int)strlen(buf->lines[r]);
    char *last = NULL;
    char *p = buf->lines[r];
    while ((p = strstr(p, pattern)) != NULL) {
      int off = (int)(p - buf->lines[r]);
      if (off < end) { last = p; p++; }
      else break;
    }
    if (last) {
      *out_row = r;
      *out_col = (int)(last - buf->lines[r]);
      return 1;
    }
  }
  // wrap to bottom
  for (int r = buf->line_count - 1; r > start_row; r--) {
    int end = (int)strlen(buf->lines[r]);
    char *last = NULL;
    char *p = buf->lines[r];
    while ((p = strstr(p, pattern)) != NULL) {
      int off = (int)(p - buf->lines[r]);
      if (off < end) { last = p; p++; }
      else break;
    }
    if (last) {
      *out_row = r;
      *out_col = (int)(last - buf->lines[r]);
      return 1;
    }
  }
  return 0;
}

int is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

int classify_char(char *line, int col) {
  char c = line[col];

  // Check if inside // comment
  for (int i = 1; i <= col; i++) {
    if (i >= 1 && line[i - 1] == '/' && line[i] == '/')
      return 2; // gray
  }

  // Check if inside string
  int in_string = 0;
  for (int i = 0; i < col; i++) {
    if (line[i] == '"')
      in_string = !in_string;
  }
  if (in_string || c == '"')
    return 3; // green

  // Number
  if (c >= '0' && c <= '9' && (col == 0 || !is_word_char(line[col - 1])))
    return 4; // bronze

  // Keywords
  char *keywords[] = {
      "int",     "void",    "char",     "if",     "else",     "for",
      "while",   "return",  "struct",   "static", "const",    "sizeof",
      "typedef", "NULL",    "enum",     "break",  "continue", "switch",
      "case",    "default", "unsigned", "signed", "long",     "short",
      "float",   "double",  "include",  "define", "main",     "printf",
      "malloc",  "calloc",  "realloc",  "free",   "fopen",    "fclose",
      "fgets",   "fprintf", "strlen",   "strcpy", "strcmp",   "memcpy",
      "memmove", "memset",  "snprintf", "FILE",   NULL};
  if (is_word_char(c)) {
    // Walk backward to find the start of this word
    int word_start = col;
    while (word_start > 0 && is_word_char(line[word_start - 1]))
      word_start--;
    // Then check if that word is a keyword
    for (int k = 0; keywords[k]; k++) {
      int kw_len = strlen(keywords[k]);
      if (strncmp(line + word_start, keywords[k], kw_len) == 0 &&
          !is_word_char(line[word_start + kw_len]))
        return 1;
    }
  }

  return 0; // white
}

int get_word_prefix_start(char *line, int col) {
  while (col > 0 && is_word_char(line[col - 1])) col--;
  return col;
}

void filter_completions(char *line, int col) {
  int ws = get_word_prefix_start(line, col);
  int prefix_len = col - ws;
  completion_count = 0;
  for (int i = 0; i < base_count; i++) {
    if (prefix_len == 0 ||
        strncmp(base_completions[i].insert, line + ws, prefix_len) == 0 ||
        strncmp(base_completions[i].label, line + ws, prefix_len) == 0) {
      completions[completion_count] = base_completions[i];
      completion_count++;
    }
  }
  if (completion_selected >= completion_count)
    completion_selected = completion_count > 0 ? completion_count - 1 : 0;
}

void delete_inside(Buffer *buf, int *row, int *col, char pair) {
  char open, close;
  switch (pair) {
    case '(': case ')': open = '('; close = ')'; break;
    case '{': case '}': open = '{'; close = '}'; break;
    case '[': case ']': open = '['; close = ']'; break;
    case '"': open = '"'; close = '"'; break;
    case '\'': open = '\''; close = '\''; break;
    default: return;
  }
  int open_pos = -1, depth = 0;
  for (int i = *col - 1; i >= 0; i--) {
    if (open == close) {
      if (buf->lines[*row][i] == open) { open_pos = i; break; }
    } else {
      if (buf->lines[*row][i] == close) depth++;
      else if (buf->lines[*row][i] == open) {
        if (depth == 0) { open_pos = i; break; }
        depth--;
      }
    }
  }
  if (open_pos < 0) return;
  int close_pos = -1;
  depth = 0;
  for (int i = open_pos + 1; buf->lines[*row][i]; i++) {
    if (open == close) {
      if (buf->lines[*row][i] == close) { close_pos = i; break; }
    } else {
      if (buf->lines[*row][i] == open) depth++;
      else if (buf->lines[*row][i] == close) {
        if (depth == 0) { close_pos = i; break; }
        depth--;
      }
    }
  }
  if (close_pos < 0) return;
  int count = close_pos - open_pos - 1;
  for (int i = 0; i < count; i++)
    buffer_delete_char(buf, *row, open_pos + 2);
  *col = open_pos + 1;
  if (*col < 0) *col = 0;
}

static int entry_cmp(const void *a, const void *b) {
  return strcmp(*(const char **)a, *(const char **)b);
}

void load_directory(Buffer *buf, const char *path) {
  DIR *d = opendir(path);
  if (!d) return;
  char **entries = NULL;
  int count = 0, cap = 0;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (count >= cap) {
      cap = cap ? cap * 2 : 64;
      entries = realloc(entries, sizeof(char *) * cap);
    }
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
    struct stat st;
    int is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
    char *line = malloc(strlen(entry->d_name) + 2);
    sprintf(line, "%s%c", entry->d_name, is_dir ? '/' : ' ');
    entries[count++] = line;
  }
  closedir(d);
  qsort(entries, count, sizeof(char *), entry_cmp);
  int total = count + 1;
  char **all = malloc(sizeof(char *) * total);
  all[0] = strdup("../");
  for (int i = 0; i < count; i++) all[i + 1] = entries[i];
  free(entries);
  for (int i = 0; i < buf->line_count; i++) free(buf->lines[i]);
  free(buf->lines);
  buf->capacity = total;
  buf->lines = all;
  buf->line_count = total;
}

int main(int argc, char *argv[]) {
  Cursor cursor;
  Buffer buf;

  char filename[512] = "";
  if (argc > 1) {
    strncpy(filename, argv[1], sizeof(filename) - 1);
    buf = buffer_load(filename);
  } else {
    buf = buffer_create(300);
  }
  LspClient *lsp = NULL;
  if (filename[0]) {
    lsp = lsp_init(filename);
    if (lsp)
      lsp_open(lsp, &buf);
  }

  User user = create_user();

  int exploring = 0;
  char explore_dir[512] = "";

  int cursor_row = 0;
  int cursor_col = 0;
  int cursor_col_target = 0;

  int scroll_row = 0;
  SDL_Init(SDL_INIT_VIDEO);
  TTF_Init();

  SDL_Window *window = SDL_CreateWindow("Zenk", 800, 800, SDL_WINDOW_RESIZABLE);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, 0);

  TTF_Font *font = TTF_OpenFont("./fonts/code_font.ttf", 16);

  // int glyph_w, glyph_h;
  TTF_GetStringSize(font, "W", 1, &CHAR_WIDTH, &CHAR_HEIGHT);
  GUTTER_WIDTH = 4 * CHAR_WIDTH;

  SDL_Surface *atlas =
      SDL_CreateSurface(CHAR_WIDTH * 95, CHAR_HEIGHT, SDL_PIXELFORMAT_RGBA8888);

  for (int i = 32; i < 128; i++) {
    char c[2] = {(char)i, '\0'};
    SDL_Surface *glyph =
        TTF_RenderText_Shaded(font, c, 1, (SDL_Color){255, 255, 255, 255},
                              (SDL_Color){20, 20, 20, 255});
    SDL_Rect dst = {(i - 32) * CHAR_WIDTH, 0, CHAR_WIDTH, CHAR_HEIGHT};
    SDL_BlitSurface(glyph, NULL, atlas, &dst);
    SDL_DestroySurface(glyph);
  }

  SDL_Texture *fontTexture = SDL_CreateTextureFromSurface(renderer, atlas);

  SDL_DestroySurface(atlas);

  SDL_StartTextInput(window);

  bool running = true;
  SDL_Event event;
  // Command buffer
  char cmd_buf[256] = {0};
  int cmd_len = 0;

  Uint32 blink_timer = SDL_GetTicks();
  bool cursor_visible = true;

  // ECHO BUG FIXING
  Uint64 swallow_until = 0;
  // Vim Keymaps needed
  char pending_operator = 0;
  char pending_motion = 0;
  char *yank_buffer = NULL;
  // Visual Mode
  int visual_anchor_row = 0;
  int visual_anchor_col = 0;
  // Prefix Counting
  int prefix_count = 0;
  // Key debounce
  Uint64 last_tab_time = 0;
  Uint64 last_bksp_time = 0;
  Uint64 last_enter_time = 0;
  Uint64 last_completion_time = 0;
  Uint64 last_comp_nav_time = 0;
  Uint64 save_feedback_time = 0;
  int pending_g = 0;
  char last_search[256] = "";
  char status_msg[512] = "";
  Uint64 status_msg_time = 0;
  // Main Loop
  while (running) {
    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    int text_area_height = h - CHAR_HEIGHT;
    int max_visible_lines = text_area_height / CHAR_HEIGHT;

    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) {
        running = false;
      }
      if (event.type == SDL_EVENT_TEXT_INPUT) {
        cursor_visible = true;
        blink_timer = SDL_GetTicks();
        if (user.state == INSERT) {
          if (swallow_until && SDL_GetTicks() < swallow_until) {
            swallow_until = 0;
          } else {
            char c = event.text.text[0];
            if (c == '(' || c == '{' || c == '[') {
              char close = c == '(' ? ')' : c == '{' ? '}' : ']';
              buffer_insert_char(&buf, cursor_row, cursor_col, c);
              cursor_col++;
              buffer_insert_char(&buf, cursor_row, cursor_col, close);
              cursor_col--;
              if (lsp) lsp_change(lsp, &buf);
            } else if (c == '"' || c == '\'') {
              buffer_insert_char(&buf, cursor_row, cursor_col, c);
              cursor_col++;
              buffer_insert_char(&buf, cursor_row, cursor_col, c);
              cursor_col--;
              if (lsp) lsp_change(lsp, &buf);
              if (base_count > 0)
                filter_completions(buf.lines[cursor_row], cursor_col);
            } else if (c >= 32) {
              buffer_insert_char(&buf, cursor_row, cursor_col, c);
              cursor_col++;
              if (lsp) lsp_change(lsp, &buf);
              if (base_count > 0)
                filter_completions(buf.lines[cursor_row], cursor_col);
              if (lsp && completion_count == 0 &&
                  SDL_GetTicks() - last_completion_time > 120) {
                lsp_request_completion(lsp, cursor_row, cursor_col);
                last_completion_time = SDL_GetTicks();
              }
            }
          }
        } else if (user.state == NORMAL && pending_motion == 'i' &&
                   strchr("(){}[]\"'", event.text.text[0])) {
          push_undo(&buf, cursor_row, cursor_col);
          delete_inside(&buf, &cursor_row, &cursor_col, event.text.text[0]);
          cursor_col_target = cursor_col;
          pending_motion = 0;
          if (lsp) lsp_change(lsp, &buf);
        } else if (user.state == NORMAL && event.text.text[0] == '$') {
          cursor_col = strlen(buf.lines[cursor_row]);
          cursor_col_target = cursor_col;
        } else if (user.state == NORMAL && event.text.text[0] == '^') {
          cursor_col = 0;
          while (buf.lines[cursor_row][cursor_col] == ' ') cursor_col++;
          cursor_col_target = cursor_col;
        } else if (user.state == NORMAL && event.text.text[0] == ':') {
          user.state = COMMAND;
          cmd_len = 1;
          cmd_buf[0] = ':';
          cmd_buf[1] = '\0';
        } else if (user.state == NORMAL && event.text.text[0] == '/') {
          user.state = COMMAND;
          cmd_len = 1;
          cmd_buf[0] = '/';
          cmd_buf[1] = '\0';
        } else if (user.state == COMMAND && cmd_len < 255 &&
                   event.text.text[0] >= 32) {
          cmd_buf[cmd_len++] = event.text.text[0];
          cmd_buf[cmd_len] = '\0';
        }
      }
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
          event.button.button == SDL_BUTTON_LEFT) {
        int mx = event.button.x;
        int my = event.button.y;
        int col = (mx - GUTTER_WIDTH) / CHAR_WIDTH;
        if (col < 0) col = 0;
        int row = my / CHAR_HEIGHT + scroll_row;
        if (row >= buf.line_count) row = buf.line_count - 1;
        if (row < 0) row = 0;
        int line_len = (int)strlen(buf.lines[row]);
        if (col > line_len) col = line_len;
        cursor_row = row;
        cursor_col = col;
        cursor_col_target = col;
        if (user.state == COMMAND) {
          // Clicking in the status bar area focuses command input
        } else if (user.state != INSERT) {
          user.state = NORMAL;
        }
      }
      if (event.type == SDL_EVENT_KEY_DOWN) {
        cursor_visible = true;
        blink_timer = SDL_GetTicks();
        if (event.key.key == SDLK_ESCAPE) {
          user.state = NORMAL;
          base_count = 0;
          completion_count = 0;
        }
        if (event.key.key == SDLK_LEFT) {
          if (cursor_col > 0) {
            cursor_col--;
          }
          cursor_col_target = cursor_col;
        }
        if (event.key.key == SDLK_RIGHT) {
          if (cursor_col < (int)strlen(buf.lines[cursor_row])) {
            cursor_col++;
          }
          cursor_col_target = cursor_col;
        }
        if (event.key.key == SDLK_DOWN) {
          if (cursor_row != buf.line_count - 1) {
            cursor_row++;
          }
          int line_len = strlen(buf.lines[cursor_row]);
          cursor_col = min(cursor_col_target, line_len);
        }
        if (event.key.key == SDLK_UP) {
          if (cursor_row != 0) {
            cursor_row--;
          }
          int line_len = strlen(buf.lines[cursor_row]);
          cursor_col = min(cursor_col_target, line_len);
        }

        if (event.key.key == SDLK_HOME) {
          cursor_col = 0;
          cursor_col_target = 0;
        }
        if (event.key.key == SDLK_END) {
          cursor_col = strlen(buf.lines[cursor_row]);
        }
        if (event.key.key == SDLK_PAGEUP) {
          scroll_row -= max_visible_lines;
          if (scroll_row < 0) scroll_row = 0;
          cursor_row = scroll_row;
          if (cursor_row < 0) cursor_row = 0;
          cursor_col_target = cursor_col;
        }
        if (event.key.key == SDLK_PAGEDOWN) {
          scroll_row += max_visible_lines;
          if (scroll_row + max_visible_lines > buf.line_count)
            scroll_row = buf.line_count - max_visible_lines;
          if (scroll_row < 0) scroll_row = 0;
          cursor_row = scroll_row;
          if (cursor_row >= buf.line_count) cursor_row = buf.line_count - 1;
          cursor_col_target = cursor_col;
        }
        if (user.state != INSERT) {

          if (event.key.key == SDLK_H) {
            if (cursor_col > 0) {
              cursor_col--;
            }
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_L) {
            if (cursor_col < (int)strlen(buf.lines[cursor_row])) {
              cursor_col++;
            }
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_K && !(event.key.mod & SDL_KMOD_SHIFT)) {
            if (cursor_row != 0) {
              cursor_row--;
            }
            int line_len = strlen(buf.lines[cursor_row]);
            cursor_col = min(cursor_col_target, line_len);
          }
          if (event.key.key == SDLK_J && !(event.key.mod & SDL_KMOD_SHIFT)) {
            int count = prefix_count > 0 ? prefix_count : 1;
            for (int c = 0; c < count; c++) {
              if (cursor_row != buf.line_count - 1) {
                cursor_row++;
              }
            }
            int line_len = strlen(buf.lines[cursor_row]);
            cursor_col = min(cursor_col_target, line_len);
            prefix_count = 0;
          }
        }
        if (user.state == NORMAL) {
          if (pending_operator != 0 && event.key.key != SDLK_D &&
              event.key.key != SDLK_Y) {
            pending_operator = 0;
            pending_motion = 0;
          }
          if (event.key.key == SDLK_U) {
            undo(&buf, &cursor_row, &cursor_col);
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_R && (event.key.mod & SDL_KMOD_CTRL)) {
            redo(&buf, &cursor_row, &cursor_col);
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_W) {
            int line_len = strlen(buf.lines[cursor_row]);
            int pos = cursor_col;

            while (pos < line_len) {
              char c = buf.lines[cursor_row][pos];
              if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_') {
                pos++;
              } else {
                break;
              }
            }
            while (pos < line_len) {
              char c = buf.lines[cursor_row][pos];
              if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_')) {
                pos++;
              } else {
                break;
              }
            }
            if (pos < line_len) {
              cursor_col = pos;
              cursor_col_target = pos;
            } else if (cursor_row < buf.line_count - 1) {
              cursor_row++;
              cursor_col = 0;
              cursor_col_target = 0;
            }
          }
          if (event.key.key == SDLK_B) {
            if (cursor_col > 0) {
              int pos = cursor_col - 1;

              // Skip non-word chars backward
              while (pos > 0) {
                char c = buf.lines[cursor_row][pos];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_'))
                  pos--;
                else
                  break;
              }

              // Skip word chars backward to find start of word
              while (pos > 0) {
                char c = buf.lines[cursor_row][pos - 1];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_')
                  pos--;
                else
                  break;
              }

              cursor_col = pos;
              cursor_col_target = pos;
            }
          }
          if (event.key.key >= SDLK_1 && event.key.key <= SDLK_9 &&
              !(event.key.mod & SDL_KMOD_SHIFT)) {
            prefix_count = prefix_count * 10 + (event.key.key - SDLK_1 + 1);
          }
          if (event.key.key == SDLK_0 && !(event.key.mod & SDL_KMOD_SHIFT) &&
              prefix_count == 0) {
            cursor_col = 0;
            cursor_col_target = 0;
          } else if (event.key.key == SDLK_0 &&
                     !(event.key.mod & SDL_KMOD_SHIFT)) {
            prefix_count *= 10;
          }

          // SWITCHING MODE TO INSERT / di motion
          if (event.key.key == SDLK_I) {
            if (pending_operator == 'd') {
              pending_motion = 'i';
              pending_operator = 0;
            } else {
              push_undo(&buf, cursor_row, cursor_col);
              user.state = INSERT;
              swallow_until = SDL_GetTicks() + 50;
            }
          }
          // SWITCHING MODE TO VISUAL
          if (event.key.key == SDLK_V) {
            visual_anchor_row = cursor_row;
            visual_anchor_col = cursor_col;
            user.state = VISUAL;
          }
          if (event.key.key == SDLK_X && (event.key.mod & SDL_KMOD_SHIFT)) {
            // X — delete character backward
            push_undo(&buf, cursor_row, cursor_col);
            if (cursor_col > 0) {
              buffer_delete_char(&buf, cursor_row, cursor_col);
              cursor_col--;
              lsp_change(lsp, &buf);
            }
          } else if (event.key.key == SDLK_X) {
            // x — delete character forward
            push_undo(&buf, cursor_row, cursor_col);
            int count = prefix_count > 0 ? prefix_count : 1;
            for (int c = 0; c < count; c++) {
              int line_len = strlen(buf.lines[cursor_row]);
              if (cursor_col >= line_len) break;
              buffer_delete_char(&buf, cursor_row, cursor_col + 1);
            }
            prefix_count = 0;
            lsp_change(lsp, &buf);
          }
        }
        if (event.key.key == SDLK_D) {
          if (pending_operator == 'd') {
            push_undo(&buf, cursor_row, cursor_col);
            int count = prefix_count > 0 ? prefix_count : 1;
            for (int c = 0; c < count; c++) {
              if (buf.line_count <= 1) break;
              buffer_delete_line(&buf, cursor_row);
              if (cursor_row >= buf.line_count)
                cursor_row = buf.line_count - 1;
            }
            cursor_col = 0;
            if (lsp) lsp_change(lsp, &buf);
            pending_operator = 0;
            prefix_count = 0;
          } else {
            pending_operator = 'd';
          }
        } else if (event.key.key == SDLK_Y) {
          if (pending_operator == 'y') {
            free(yank_buffer);
            yank_buffer = malloc(strlen(buf.lines[cursor_row]) + 1);
            strcpy(yank_buffer, buf.lines[cursor_row]);
            pending_operator = 0;
          } else {
            pending_operator = 'y';
          }
        } else {
          pending_operator = 0;
          pending_motion = 0;
        }
        if (user.state == NORMAL) {
          // --- Paste Command ---
          if (event.key.key == SDLK_P) {
            if (yank_buffer) {
              push_undo(&buf, cursor_row, cursor_col);
              buffer_insert_line(&buf, cursor_row + 1);
              free(buf.lines[cursor_row + 1]);
              buf.lines[cursor_row + 1] = malloc(strlen(yank_buffer) + 1);
              strcpy(buf.lines[cursor_row + 1], yank_buffer);
              cursor_row++;
              cursor_col = 0;
              if (lsp) {
                lsp_change(lsp, &buf);
              }
            }
          }
          if (event.key.key == SDLK_O && (event.key.mod & SDL_KMOD_SHIFT)) {
            // O — open line ABOVE
            push_undo(&buf, cursor_row, cursor_col);
            buffer_insert_line(&buf, cursor_row);
            user.state = INSERT;
            cursor_col = 0;
            swallow_until = SDL_GetTicks() + 50;
            if (lsp)
              lsp_change(lsp, &buf);
          } else if (event.key.key == SDLK_O) {
            push_undo(&buf, cursor_row, cursor_col);
            buffer_insert_line(&buf, cursor_row + 1);
            user.state = INSERT;
            cursor_row++;
            cursor_col = 0;
            swallow_until = SDL_GetTicks() + 50;
            if (lsp)
              lsp_change(lsp, &buf);
          }
          if (event.key.key == SDLK_A && (event.key.mod & SDL_KMOD_SHIFT)) {
            // A — append at end of line
            push_undo(&buf, cursor_row, cursor_col);
            cursor_col = strlen(buf.lines[cursor_row]);
            user.state = INSERT;
            swallow_until = SDL_GetTicks() + 50;
          } else if (event.key.key == SDLK_A) {
            // a — append after cursor
            push_undo(&buf, cursor_row, cursor_col);
            if (cursor_col < (int)strlen(buf.lines[cursor_row]))
              cursor_col++;
            user.state = INSERT;
            swallow_until = SDL_GetTicks() + 50;
          }
          // K — hover info
          if (event.key.key == SDLK_K && (event.key.mod & SDL_KMOD_SHIFT)) {
            if (lsp) lsp_request_hover(lsp, cursor_row, cursor_col);
          }
          // gg → first line, gd → go to definition
          if (event.key.key == SDLK_G && !(event.key.mod & SDL_KMOD_SHIFT)) {
            if (pending_g) {
              cursor_row = 0;
              cursor_col = 0;
              cursor_col_target = 0;
              pending_g = 0;
            } else {
              pending_g = 1;
            }
          } else if (pending_g && event.key.key == SDLK_D) {
            pending_g = 0;
            if (lsp) lsp_request_definition(lsp, cursor_row, cursor_col);
          } else {
            pending_g = 0;
          }
          // n / N — search next/prev
          if (event.key.key == SDLK_N && last_search[0]) {
            int nr, nc;
            int shift = (event.key.mod & SDL_KMOD_SHIFT);
            int found = shift ? search_backward(&buf, last_search, cursor_row, cursor_col, &nr, &nc)
                              : search_forward(&buf, last_search, cursor_row, cursor_col + 1, &nr, &nc);
            if (found) { cursor_row = nr; cursor_col = nc; cursor_col_target = nc;
              snprintf(status_msg, sizeof(status_msg), "/%s", last_search);
              status_msg_time = SDL_GetTicks();
            } else {
              snprintf(status_msg, sizeof(status_msg), "Pattern not found: %s", last_search);
              status_msg_time = SDL_GetTicks();
            }
          }
          // Ctrl+D — half page down, Ctrl+U — half page up
          if (event.key.key == SDLK_D && (event.key.mod & SDL_KMOD_CTRL)) {
            int half = max_visible_lines / 2;
            cursor_row += half;
            if (cursor_row >= buf.line_count) cursor_row = buf.line_count - 1;
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_U && (event.key.mod & SDL_KMOD_CTRL)) {
            int half = max_visible_lines / 2;
            cursor_row -= half;
            if (cursor_row < 0) cursor_row = 0;
            cursor_col_target = cursor_col;
          }
          // G — go to last line (or line N with prefix)
          if (event.key.key == SDLK_G && (event.key.mod & SDL_KMOD_SHIFT)) {
            if (prefix_count > 0) {
              cursor_row = prefix_count - 1;
              if (cursor_row >= buf.line_count) cursor_row = buf.line_count - 1;
              prefix_count = 0;
            } else {
              cursor_row = buf.line_count - 1;
            }
            cursor_col = 0;
            cursor_col_target = 0;
          }
          // J — join next line
          if (event.key.key == SDLK_J && (event.key.mod & SDL_KMOD_SHIFT)) {
            if (cursor_row < buf.line_count - 1) {
              push_undo(&buf, cursor_row, cursor_col);
              int cur_len = strlen(buf.lines[cursor_row]);
              int next_len = strlen(buf.lines[cursor_row + 1]);
              buf.lines[cursor_row] =
                  realloc(buf.lines[cursor_row], cur_len + next_len + 1);
              memcpy(buf.lines[cursor_row] + cur_len,
                     buf.lines[cursor_row + 1], next_len + 1);
              buffer_delete_line(&buf, cursor_row + 1);
              lsp_change(lsp, &buf);
            }
          }
        }
      }
      // Explorer mode: Enter opens file/dir, - goes up
      if (exploring && user.state == NORMAL) {
        if (event.key.key == SDLK_RETURN && !event.key.repeat &&
            SDL_GetTicks() - last_enter_time > 100) {
          last_enter_time = SDL_GetTicks();
          char *line = buf.lines[cursor_row];
          int len = strlen(line);
          if (len > 0 && line[len - 1] == '/') {
            char new_path[1024];
            line[len - 1] = '\0';
            snprintf(new_path, sizeof(new_path), "%s/%s", explore_dir, line);
            line[len - 1] = '/';
            strncpy(explore_dir, new_path, sizeof(explore_dir) - 1);
            load_directory(&buf, explore_dir);
            cursor_row = 0;
            cursor_col = 0;
          } else {
            char full_path[1024];
            // Trim trailing space from file entries
            int flen = len;
            while (flen > 0 && line[flen - 1] == ' ') flen--;
            snprintf(full_path, sizeof(full_path), "%s/%.*s",
                     explore_dir, flen, line);
            if (lsp) { lsp_close(lsp); lsp_destroy(lsp); lsp = NULL; }
            buffer_destroy(&buf);
            buf = buffer_load(full_path);
            strncpy(filename, full_path, sizeof(filename) - 1);
            lsp = lsp_init(filename);
            if (lsp) lsp_open(lsp, &buf);
            exploring = 0;
            cursor_row = 0;
            cursor_col = 0;
            cursor_col_target = 0;
            scroll_row = 0;
          }
        }
        if (event.key.key == SDLK_MINUS && !event.key.repeat) {
          char *slash = strrchr(explore_dir, '/');
          if (slash && slash != explore_dir) {
            *slash = '\0';
          } else if (slash == explore_dir) {
            explore_dir[1] = '\0';
          } else {
            strncpy(explore_dir, "..", sizeof(explore_dir) - 1);
          }
          load_directory(&buf, explore_dir);
          cursor_row = 0;
          cursor_col = 0;
        }
      }
      if (user.state == INSERT) {
        if (completion_count > 0 &&
            event.key.key == SDLK_DOWN &&
            SDL_GetTicks() - last_comp_nav_time > 100) {
          last_comp_nav_time = SDL_GetTicks();
          completion_selected =
              (completion_selected + 1) % completion_count;
        } else if (completion_count > 0 &&
                   event.key.key == SDLK_TAB &&
                   !(event.key.mod & SDL_KMOD_SHIFT) &&
                   SDL_GetTicks() - last_comp_nav_time > 100) {
          last_comp_nav_time = SDL_GetTicks();
          completion_selected =
              (completion_selected + 1) % completion_count;
        } else if (completion_count > 0 &&
                   ((event.key.key == SDLK_TAB &&
                     (event.key.mod & SDL_KMOD_SHIFT)) ||
                    event.key.key == SDLK_UP) &&
                   SDL_GetTicks() - last_comp_nav_time > 100) {
          last_comp_nav_time = SDL_GetTicks();
          completion_selected =
              (completion_selected - 1 + completion_count) % completion_count;
        } else if (completion_count > 0 && event.key.key == SDLK_RETURN && !event.key.repeat) {
          last_enter_time = SDL_GetTicks();
          CompletionItem *ci = &completions[completion_selected];
          char *ins = ci->insert;
          int ws = get_word_prefix_start(buf.lines[cursor_row], cursor_col);
          for (int i = ws; i < cursor_col; i++)
            buffer_delete_char(&buf, cursor_row, ws + 1);
          cursor_col = ws;
          for (int i = 0; ins[i]; i++) {
            buffer_insert_char(&buf, cursor_row, cursor_col, ins[i]);
            cursor_col++;
          }
          if (lsp) lsp_change(lsp, &buf);
          base_count = 0;
          completion_count = 0;
        } else if (completion_count > 0 && event.key.key == SDLK_ESCAPE) {
          base_count = 0;
          completion_count = 0;
        } else if (event.key.key == SDLK_TAB &&
                   SDL_GetTicks() - last_tab_time > 100) {
          last_tab_time = SDL_GetTicks();
          buffer_insert_char(&buf, cursor_row, cursor_col, ' ');
          cursor_col++;
          buffer_insert_char(&buf, cursor_row, cursor_col, ' ');
          cursor_col++;
          swallow_until = SDL_GetTicks() + 50;
          if (lsp) {
            lsp_change(lsp, &buf);
          }
        } else if (event.key.key == SDLK_RETURN &&
                   !event.key.repeat &&
                   completion_count == 0 &&
                   SDL_GetTicks() - last_enter_time > 100) {
          last_enter_time = SDL_GetTicks();
          int len = strlen(buf.lines[cursor_row]);

          int indent = 0;
          while (buf.lines[cursor_row][indent] == ' ' ||
                 buf.lines[cursor_row][indent] == '\t') {
            indent++;
          }
          int tail_len = len - cursor_col;
          buffer_insert_line(&buf, cursor_row + 1);

          char *new_line = malloc(indent + tail_len + 1);
          memcpy(new_line, buf.lines[cursor_row], indent);
          memcpy(new_line + indent, buf.lines[cursor_row] + cursor_col,
                 tail_len);
          new_line[indent + tail_len] = '\0';
          buf.lines[cursor_row + 1] = new_line;

          buf.lines[cursor_row] =
              realloc(buf.lines[cursor_row], cursor_col + 1);
          buf.lines[cursor_row][cursor_col] = '\0';

          cursor_row++;
          cursor_col = indent;
          swallow_until = SDL_GetTicks() + 50;
          if (lsp) {
            lsp_change(lsp, &buf);
          }
        } else if (event.key.key == SDLK_BACKSPACE &&
                   !event.key.repeat &&
                   SDL_GetTicks() - last_bksp_time > 100) {
           last_bksp_time = SDL_GetTicks();
          if (cursor_col > 0) {
            // Empty auto-pair: delete both brackets
            if (cursor_col < (int)strlen(buf.lines[cursor_row]) &&
                ((buf.lines[cursor_row][cursor_col - 1] == '(' &&
                  buf.lines[cursor_row][cursor_col] == ')') ||
                 (buf.lines[cursor_row][cursor_col - 1] == '{' &&
                  buf.lines[cursor_row][cursor_col] == '}') ||
                 (buf.lines[cursor_row][cursor_col - 1] == '[' &&
                  buf.lines[cursor_row][cursor_col] == ']') ||
                 (buf.lines[cursor_row][cursor_col - 1] == '"' &&
                  buf.lines[cursor_row][cursor_col] == '"') ||
                 (buf.lines[cursor_row][cursor_col - 1] == '\'' &&
                  buf.lines[cursor_row][cursor_col] == '\''))) {
              buffer_delete_char(&buf, cursor_row, cursor_col + 1);
              cursor_col--;
              buffer_delete_char(&buf, cursor_row, cursor_col + 1);
              if (lsp) lsp_change(lsp, &buf);
            } else {
              buffer_delete_char(&buf, cursor_row, cursor_col);
              cursor_col--;
              if (lsp) lsp_change(lsp, &buf);
            }
          } else if (cursor_row > 0) {
            int prev_len = strlen(buf.lines[cursor_row - 1]);
            int cur_len = strlen(buf.lines[cursor_row]);
            buf.lines[cursor_row - 1] =
                realloc(buf.lines[cursor_row - 1], prev_len + cur_len + 1);
            memcpy(buf.lines[cursor_row - 1] + prev_len,
                   buf.lines[cursor_row], cur_len + 1);
            buffer_delete_line(&buf, cursor_row);
            cursor_row--;
            cursor_col = prev_len;
            if (lsp) lsp_change(lsp, &buf);
          }
          if (base_count > 0)
            filter_completions(buf.lines[cursor_row], cursor_col);
        } else if (event.key.key == SDLK_DELETE && !event.key.repeat) {
          // Del — delete forward
          int line_len = strlen(buf.lines[cursor_row]);
          if (cursor_col < line_len) {
            buffer_delete_char(&buf, cursor_row, cursor_col + 1);
            if (lsp) lsp_change(lsp, &buf);
          } else if (cursor_row < buf.line_count - 1) {
            int cur_len = strlen(buf.lines[cursor_row]);
            int next_len = strlen(buf.lines[cursor_row + 1]);
            buf.lines[cursor_row] =
                realloc(buf.lines[cursor_row], cur_len + next_len + 1);
            memcpy(buf.lines[cursor_row] + cur_len,
                   buf.lines[cursor_row + 1], next_len + 1);
            buffer_delete_line(&buf, cursor_row + 1);
            if (lsp) lsp_change(lsp, &buf);
          }
        } else if (event.key.key == SDLK_W && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+w — delete word backward
          int ws = cursor_col;
          while (ws > 0 && buf.lines[cursor_row][ws - 1] == ' ') ws--;
          while (ws > 0 && is_word_char(buf.lines[cursor_row][ws - 1])) ws--;
          for (int i = ws; i < cursor_col; i++)
            buffer_delete_char(&buf, cursor_row, ws + 1);
          cursor_col = ws;
          swallow_until = SDL_GetTicks() + 50;
          if (lsp) lsp_change(lsp, &buf);
        } else if (event.key.key == SDLK_U && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+u — delete to start of line
          for (int i = 0; i < cursor_col; i++)
            buffer_delete_char(&buf, cursor_row, 1);
          cursor_col = 0;
          swallow_until = SDL_GetTicks() + 50;
          if (lsp) lsp_change(lsp, &buf);
        } else if (event.key.key == SDLK_S && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+s — save
          buffer_save(&buf, filename);
          save_feedback_time = SDL_GetTicks();
          swallow_until = SDL_GetTicks() + 50;
        } else if (event.key.key == SDLK_C && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+c — exit to NORMAL
          user.state = NORMAL;
          swallow_until = SDL_GetTicks() + 50;
        } else if (event.key.key == SDLK_SPACE && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+Space — manual completion trigger
          if (lsp) {
            lsp_request_completion(lsp, cursor_row, cursor_col);
            last_completion_time = SDL_GetTicks();
          }
        }
      }
      if (user.state == VISUAL) {
        if (event.key.key == SDLK_D || event.key.key == SDLK_X) {
          push_undo(&buf, cursor_row, cursor_col);
          // Normalize selection range (smaller → larger)
          int sr = min(visual_anchor_row, cursor_row);
          int er = max(visual_anchor_row, cursor_row);
          int sc = (visual_anchor_row == cursor_row)
                       ? min(visual_anchor_col, cursor_col)
                       : (visual_anchor_row < cursor_row ? visual_anchor_col
                                                         : cursor_col);
          int ec = (visual_anchor_row == cursor_row)
                       ? max(visual_anchor_col, cursor_col)
                       : (visual_anchor_row > cursor_row ? visual_anchor_col
                                                         : cursor_col);

          // Yank the deleted text (for pasting later)
          free(yank_buffer);
          if (sr == er) {
            // Same line: copy range, shift content left
            int range = ec - sc;
            yank_buffer = malloc(range + 1);
            memcpy(yank_buffer, buf.lines[sr] + sc, range);
            yank_buffer[range] = '\0';

            int len = strlen(buf.lines[sr]);
            memmove(buf.lines[sr] + sc, buf.lines[sr] + ec, len - ec + 1);
            buf.lines[sr] = realloc(buf.lines[sr], len - range + 1);
          } else {
            // Multi-line: copy everything into yank_buffer with \n
            int tail_len = strlen(buf.lines[sr]) - sc;
            int yank_size = tail_len + 1; // first line tail + \n
            for (int i = sr + 1; i < er; i++)
              yank_size += strlen(buf.lines[i]) + 1; // middle lines + \n
            yank_size += ec + 1;                     // end row head + null

            yank_buffer = malloc(yank_size);
            int pos = 0;
            memcpy(yank_buffer + pos, buf.lines[sr] + sc, tail_len);
            pos += tail_len;
            yank_buffer[pos++] = '\n';
            for (int i = sr + 1; i < er; i++) {
              int len = strlen(buf.lines[i]);
              memcpy(yank_buffer + pos, buf.lines[i], len);
              pos += len;
              yank_buffer[pos++] = '\n';
            }
            memcpy(yank_buffer + pos, buf.lines[er], ec);
            pos += ec;
            yank_buffer[pos] = '\0';

            // Delete: append end row's tail to start row
            int end_tail_len = strlen(buf.lines[er]) - ec;
            buf.lines[sr] = realloc(buf.lines[sr], sc + end_tail_len + 1);
            memcpy(buf.lines[sr] + sc, buf.lines[er] + ec, end_tail_len);
            buf.lines[sr][sc + end_tail_len] = '\0';

            // Free all lines from sr+1 to er
            for (int i = sr + 1; i <= er; i++)
              free(buf.lines[i]);
            // Shift remaining lines down
            int lines_to_remove = er - sr;
            memmove(&buf.lines[sr + 1], &buf.lines[er + 1],
                    sizeof(char *) * (buf.line_count - er - 1));
            buf.line_count -= lines_to_remove;
          }

          cursor_row = sr;
          cursor_col = sc;
          cursor_col_target = sc;
          user.state = NORMAL;
          if (lsp)
            lsp_change(lsp, &buf);
        }

        if (event.key.key == SDLK_Y) {
          // Same yank logic as above, but NO deletion — just copy and exit
          int sr = min(visual_anchor_row, cursor_row);
          int er = max(visual_anchor_row, cursor_row);
          int sc = (visual_anchor_row == cursor_row)
                       ? min(visual_anchor_col, cursor_col)
                       : (visual_anchor_row < cursor_row ? visual_anchor_col
                                                         : cursor_col);
          int ec = (visual_anchor_row == cursor_row)
                       ? max(visual_anchor_col, cursor_col)
                       : (visual_anchor_row > cursor_row ? visual_anchor_col
                                                         : cursor_col);

          free(yank_buffer);
          if (sr == er) {
            int range = ec - sc;
            yank_buffer = malloc(range + 1);
            memcpy(yank_buffer, buf.lines[sr] + sc, range);
            yank_buffer[range] = '\0';
          } else {
            int tail_len = strlen(buf.lines[sr]) - sc;
            int yank_size = tail_len + 1;
            for (int i = sr + 1; i < er; i++)
              yank_size += strlen(buf.lines[i]) + 1;
            yank_size += ec + 1;

            yank_buffer = malloc(yank_size);
            int pos = 0;
            memcpy(yank_buffer + pos, buf.lines[sr] + sc, tail_len);
            pos += tail_len;
            yank_buffer[pos++] = '\n';
            for (int i = sr + 1; i < er; i++) {
              int len = strlen(buf.lines[i]);
              memcpy(yank_buffer + pos, buf.lines[i], len);
              pos += len;
              yank_buffer[pos++] = '\n';
            }
            memcpy(yank_buffer + pos, buf.lines[er], ec);
            pos += ec;
            yank_buffer[pos] = '\0';
          }

          user.state = NORMAL;
        }
      }
      if (user.state == COMMAND) {
        if (event.key.key == SDLK_RETURN && !event.key.repeat) {
          last_enter_time = SDL_GetTicks();
          if (cmd_buf[0] == '/' && cmd_len > 1) {
            strncpy(last_search, cmd_buf + 1, 255);
            int nr, nc;
            if (search_forward(&buf, last_search, cursor_row, cursor_col + 1,
                               &nr, &nc)) {
              cursor_row = nr; cursor_col = nc; cursor_col_target = nc;
              snprintf(status_msg, sizeof(status_msg), "/%s", last_search);
              status_msg_time = SDL_GetTicks();
            } else {
              snprintf(status_msg, sizeof(status_msg),
                       "Pattern not found: %s", last_search);
              status_msg_time = SDL_GetTicks();
            }
          } else if (cmd_buf[0] == '/' && cmd_len == 1 && last_search[0]) {
            // // — repeat last search
            int nr, nc;
            if (search_forward(&buf, last_search, cursor_row, cursor_col + 1,
                               &nr, &nc)) {
              cursor_row = nr; cursor_col = nc; cursor_col_target = nc;
              snprintf(status_msg, sizeof(status_msg), "/%s", last_search);
              status_msg_time = SDL_GetTicks();
            } else {
              snprintf(status_msg, sizeof(status_msg),
                       "Pattern not found: %s", last_search);
              status_msg_time = SDL_GetTicks();
            }
          } else if (strcmp(cmd_buf, ":w") == 0) {
            buffer_save(&buf, filename);
            save_feedback_time = SDL_GetTicks();
          } else if (strcmp(cmd_buf, ":q") == 0 ||
                   strcmp(cmd_buf, ":q!") == 0)
            running = false;
          else if (strcmp(cmd_buf, ":wq") == 0 ||
                   strcmp(cmd_buf, ":wq!") == 0) {
            buffer_save(&buf, filename);
            save_feedback_time = SDL_GetTicks();
            running = false;
          } else if (strcmp(cmd_buf, ":w!") == 0) {
            buffer_save(&buf, filename);
            save_feedback_time = SDL_GetTicks();
          } else if (strcmp(cmd_buf, ":e") == 0) {
            strncpy(explore_dir, ".", sizeof(explore_dir) - 1);
            exploring = 1;
            if (lsp) { lsp_close(lsp); lsp_destroy(lsp); lsp = NULL; }
            buffer_destroy(&buf);
            buf = buffer_create(100);
            load_directory(&buf, explore_dir);
            cursor_row = 0;
            cursor_col = 0;
            cursor_col_target = 0;
            scroll_row = 0;
          }
          user.state = NORMAL;
        }
        if (event.key.key == SDLK_BACKSPACE && !event.key.repeat &&
            cmd_len > 0 && SDL_GetTicks() - last_bksp_time > 100) {
          last_bksp_time = SDL_GetTicks();
          cmd_buf[--cmd_len] = '\0';
        }
      }
    }

    /* ── Background ─────────────────────────────── */
    SDL_SetRenderDrawColor(renderer, 25, 23, 36, 255);
    SDL_RenderClear(renderer);

    SDL_SetTextureColorMod(fontTexture, 144, 140, 170);
    /* ── Gutter (line numbers) ──────────────────── */
    SDL_SetRenderDrawColor(renderer, 31, 29, 46, 255);
    SDL_FRect gutter_rect = {0, 0, (float)GUTTER_WIDTH, (float)h};
    SDL_RenderFillRect(renderer, &gutter_rect);

    /* ── Scroll clamp ───────────────────────────── */
    if (cursor_row < scroll_row) {
      scroll_row = cursor_row;
    }
    if (cursor_row >= scroll_row + max_visible_lines) {
      scroll_row = cursor_row - max_visible_lines + 1;
    }

    /* ── Text lines ─────────────────────────────── */
    for (int i = scroll_row; i < buf.line_count; i++) {
      int y = (i - scroll_row) * CHAR_HEIGHT;
      if (y + CHAR_HEIGHT > text_area_height)
        break;

      // draw line number
      char num_str[16];
      int rel = abs(i - cursor_row);
      snprintf(num_str, sizeof(num_str), "%d", rel);
      int num_len = strlen(num_str);
      int num_x = GUTTER_WIDTH - (num_len * CHAR_WIDTH) - CHAR_WIDTH;
      for (int j = 0; j < num_len; j++) {
        DrawChar(renderer, fontTexture, num_str[j], num_x + j * CHAR_WIDTH, y);
      }

      for (int d = 0; d < diag_count; d++) {
        if (diagnostics[d].line == i) {
          SDL_SetRenderDrawColor(renderer, 235, 111, 146, 255);
          SDL_FRect dot = {(float)CHAR_WIDTH, y + (float)CHAR_HEIGHT / 2 - 2, 6,
                           4};
          SDL_RenderFillRect(renderer, &dot);
          break;
        }
      }

      // draw text content
      int line_len = strlen(buf.lines[i]);
      for (int j = 0; j < line_len; j++) {
        // Selection highlight
        if (user.state == VISUAL) {
          int sr = min(visual_anchor_row, cursor_row);
          int er = max(visual_anchor_row, cursor_row);
          int sc = (visual_anchor_row == cursor_row)
                       ? min(visual_anchor_col, cursor_col)
                       : (visual_anchor_row < cursor_row ? visual_anchor_col
                                                         : cursor_col);
          int ec = (visual_anchor_row == cursor_row)
                       ? max(visual_anchor_col, cursor_col)
                       : (visual_anchor_row > cursor_row ? visual_anchor_col
                                                         : cursor_col);
          bool selected = false;
          if (i > sr && i < er) {
            selected = true;
          } else if (i == sr && i == er) {
            if (j >= sc && j < ec)
              selected = true;
          } else if (i == sr) {
            if (j >= sc)
              selected = true;
          } else if (i == er) {
            if (j < ec)
              selected = true;
          }
          if (selected) {
            SDL_SetRenderDrawColor(renderer, 64, 61, 82, 255);
            SDL_FRect bg = {GUTTER_WIDTH + j * CHAR_WIDTH, y, CHAR_WIDTH,
                            CHAR_HEIGHT};
            SDL_RenderFillRect(renderer, &bg);
          }
        }
        int color = classify_char(buf.lines[i], j);
        switch (color) {
        case 1:
          SDL_SetTextureColorMod(fontTexture, 196, 167, 231);
          break;
        case 2:
          SDL_SetTextureColorMod(fontTexture, 144, 140, 170);
          break;
        case 3:
          SDL_SetTextureColorMod(fontTexture, 156, 207, 216);
          break;
        case 4:
          SDL_SetTextureColorMod(fontTexture, 246, 193, 119);
          break;
        default:
          SDL_SetTextureColorMod(fontTexture, 224, 222, 244);
          break;
        }
        DrawChar(renderer, fontTexture, buf.lines[i][j],
                 GUTTER_WIDTH + j * CHAR_WIDTH, y);
        SDL_SetTextureColorMod(fontTexture, 224, 222, 244);
      }
    }

    /* ── LSP poll ──────────────────────────────── */
    if (lsp) {
      JsonValue *pending = lsp_get_pending_result(lsp);
      if (pending) {
        JsonValue *result = json_get(pending, "result");
        if (result) {
          // Hover response
          JsonValue *contents = json_get(result, "contents");
          if (contents) {
            JsonValue *value = json_get(contents, "value");
            if (value && value->type == JSON_STR) {
              snprintf(status_msg, sizeof(status_msg), "%s", value->string);
              status_msg_time = SDL_GetTicks();
            } else if (contents->type == JSON_STR) {
              snprintf(status_msg, sizeof(status_msg), "%s", contents->string);
              status_msg_time = SDL_GetTicks();
            }
          }
          // Definition response (array of locations)
          if (result->type == JSON_ARR) {
            JsonValue *loc = NULL;
            for (int i = 0; i < result->arr.count; i++) {
              JsonValue *item = result->arr.items[i];
              JsonValue *range = json_get(item, "range");
              JsonValue *start = range ? json_get(range, "start") : NULL;
              JsonValue *line = start ? json_get(start, "line") : NULL;
              if (line && line->type == JSON_NUM) {
                loc = result->arr.items[i];
                break;
              }
            }
            if (loc) {
              JsonValue *range = json_get(loc, "range");
              JsonValue *start = range ? json_get(range, "start") : NULL;
              JsonValue *line = start ? json_get(start, "line") : NULL;
              JsonValue *ch = start ? json_get(start, "character") : NULL;
              if (line && line->type == JSON_NUM) {
                cursor_row = (int)line->number;
                cursor_col = ch && ch->type == JSON_NUM ? (int)ch->number : 0;
                cursor_col_target = cursor_col;
                snprintf(status_msg, sizeof(status_msg),
                         "Go to definition");
                status_msg_time = SDL_GetTicks();
              }
            } else {
              snprintf(status_msg, sizeof(status_msg),
                       "No definition found");
              status_msg_time = SDL_GetTicks();
            }
          }
          // Completion response
          JsonValue *items = json_get(result, "items");
          if (items && items->type == JSON_ARR) {
            base_count = 0;
            for (int i = 0; i < items->arr.count && base_count < 128; i++) {
              JsonValue *item = items->arr.items[i];
              JsonValue *label = json_get(item, "label");
              JsonValue *detail = json_get(item, "detail");
              if (label && label->type == JSON_STR) {
                CompletionItem *ci = &base_completions[base_count];
                strncpy(ci->label, label->string, 255);
                JsonValue *te = json_get(item, "textEdit");
                JsonValue *tn = te ? json_get(te, "newText") : NULL;
                if (tn && tn->type == JSON_STR)
                  strncpy(ci->insert, tn->string, 255);
                else {
                  JsonValue *insert = json_get(item, "insertText");
                  if (insert && insert->type == JSON_STR)
                    strncpy(ci->insert, insert->string, 255);
                  else {
                    JsonValue *filter = json_get(item, "filterText");
                    if (filter && filter->type == JSON_STR)
                      strncpy(ci->insert, filter->string, 255);
                    else
                      strncpy(ci->insert, label->string, 255);
                  }
                }
                if (detail && detail->type == JSON_STR)
                  strncpy(ci->detail, detail->string, 255);
                else
                  ci->detail[0] = '\0';
                base_count++;
              }
            }
            filter_completions(buf.lines[cursor_row], cursor_col);
            completion_selected = 0;
          }
        }
        json_free(pending);
      }

      JsonValue *msg;
      while ((msg = lsp_poll_message(lsp)) != NULL) {
        JsonValue *method = json_get(msg, "method");
        if (method && method->type == JSON_STR &&
            strcmp(method->string, "textDocument/publishDiagnostics") == 0) {
          JsonValue *params = json_get(msg, "params");
          if (params && params->type == JSON_OBJ) {
            JsonValue *diags = json_get(params, "diagnostics");
            if (diags && diags->type == JSON_ARR) {
              diag_count = 0;
              for (int i = 0; i < diags->arr.count && diag_count < 256; i++) {
                JsonValue *d = diags->arr.items[i];
                JsonValue *range = json_get(d, "range");
                JsonValue *start = range ? json_get(range, "start") : NULL;
                JsonValue *line = start ? json_get(start, "line") : NULL;
                JsonValue *message = json_get(d, "message");
                JsonValue *severity = json_get(d, "severity");
                if (line && line->type == JSON_NUM) {
                  diagnostics[diag_count].line = (int)line->number;
                  diagnostics[diag_count].severity =
                      severity ? (int)severity->number : 1;
                  strncpy(diagnostics[diag_count].message,
                          message && message->type == JSON_STR ? message->string
                                                               : "",
                          255);
                  diag_count++;
                }
              }
            }
          }
        }
        json_free(msg);
      }
    }

    /* ── Cursor ─────────────────────────────────── */
    cursor.x = GUTTER_WIDTH + cursor_col * CHAR_WIDTH;
    cursor.y = (cursor_row - scroll_row) * CHAR_HEIGHT;

    if (user.state == INSERT || user.state == NORMAL) {
      if (SDL_GetTicks() - blink_timer > 530) {
        blink_timer = SDL_GetTicks();
        cursor_visible = !cursor_visible;
      }
    } else {
      cursor_visible = true;
    }

    if (cursor_visible) {
      if (user.state == INSERT) {
        // thin vertical bar
        SDL_SetRenderDrawColor(renderer, 156, 207, 216, 255);
        SDL_FRect cursor_rect = {cursor.x, cursor.y, 2.0f, (float)CHAR_HEIGHT};
        SDL_RenderFillRect(renderer, &cursor_rect);
      } else {
        // full block (NORMAL, COMMAND, VISUAL)
        SDL_SetRenderDrawColor(renderer, 224, 222, 244, 255);
        SDL_FRect cursor_rect = {cursor.x, cursor.y, (float)CHAR_WIDTH,
                                 (float)CHAR_HEIGHT};
        SDL_RenderFillRect(renderer, &cursor_rect);
      }
    }

    /* ── Status bar ─────────────────────────────── */
    float status_y = h - CHAR_HEIGHT;
    SDL_SetRenderDrawColor(renderer, 31, 29, 46, 255);
    SDL_FRect status_rect = {0, status_y, (float)w, (float)CHAR_HEIGHT};
    SDL_RenderFillRect(renderer, &status_rect);

    const char *mode_str = "";
    Uint8 mode_r = 196, mode_g = 167, mode_b = 231;
    switch (user.state) {
    case NORMAL:
      mode_str = "NORMAL";
      mode_r = 196; mode_g = 167; mode_b = 231; break;
    case INSERT:
      mode_str = "INSERT";
      mode_r = 156; mode_g = 207; mode_b = 216; break;
    case VISUAL:
      mode_str = "VISUAL";
      mode_r = 235; mode_g = 111; mode_b = 146; break;
    case COMMAND:
      mode_str = "COMMAND";
      mode_r = 246; mode_g = 193; mode_b = 119; break;
    }

    int sx = CHAR_WIDTH;
    int offset = 0;

    if (status_msg[0] && SDL_GetTicks() - status_msg_time < 2000) {
      int is_err = strstr(status_msg, "not found") != NULL;
      Uint8 r = is_err ? 235 : 196, g = is_err ? 111 : 167, b = is_err ? 146 : 231;
      SDL_SetTextureColorMod(fontTexture, r, g, b);
      for (int j = 0; status_msg[j]; j++) {
        DrawChar(renderer, fontTexture, status_msg[j],
                 sx + offset * CHAR_WIDTH, status_y);
        offset++;
      }
    } else {
      status_msg[0] = '\0';
    }

    if (offset == 0) {
      if (save_feedback_time && SDL_GetTicks() - save_feedback_time < 1500) {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "saved!  |  %s",
                 filename[0] ? filename : "(new)");
        SDL_SetTextureColorMod(fontTexture, 156, 207, 216);
        for (int j = 0; tmp[j]; j++) {
          DrawChar(renderer, fontTexture, tmp[j],
                   sx + offset * CHAR_WIDTH, status_y);
          offset++;
        }
      } else {
        save_feedback_time = 0;
      }
    }

    if (offset == 0) {
      if (user.state == COMMAND) {
        SDL_SetTextureColorMod(fontTexture, mode_r, mode_g, mode_b);
        for (int j = 0; cmd_buf[j]; j++) {
          DrawChar(renderer, fontTexture, cmd_buf[j],
                   sx + offset * CHAR_WIDTH, status_y);
          offset++;
        }
      } else if (exploring) {
        SDL_SetTextureColorMod(fontTexture, 144, 140, 170);
        for (int j = 0; explore_dir[j]; j++) {
          DrawChar(renderer, fontTexture, explore_dir[j],
                   sx + offset * CHAR_WIDTH, status_y);
          offset++;
        }
        char sep[] = "  |  ";
        for (int j = 0; sep[j]; j++) {
          DrawChar(renderer, fontTexture, sep[j],
                   sx + offset * CHAR_WIDTH, status_y);
          offset++;
        }
        SDL_SetTextureColorMod(fontTexture, mode_r, mode_g, mode_b);
        for (int j = 0; mode_str[j]; j++) {
          DrawChar(renderer, fontTexture, mode_str[j],
                   sx + offset * CHAR_WIDTH, status_y);
          offset++;
        }
      } else {
        const char *fn = filename[0] ? filename : "(new)";
        SDL_SetTextureColorMod(fontTexture, 156, 207, 216);
        for (int j = 0; fn[j]; j++) {
          DrawChar(renderer, fontTexture, fn[j],
                   sx + offset * CHAR_WIDTH, status_y);
          offset++;
        }
        char pos[64];
        snprintf(pos, sizeof(pos), "  |  Line %d, Col %d  |  ",
                 cursor_row + 1, cursor_col + 1);
        SDL_SetTextureColorMod(fontTexture, 144, 140, 170);
        for (int j = 0; pos[j]; j++) {
          DrawChar(renderer, fontTexture, pos[j],
                   sx + offset * CHAR_WIDTH, status_y);
          offset++;
        }
        SDL_SetTextureColorMod(fontTexture, mode_r, mode_g, mode_b);
        for (int j = 0; mode_str[j]; j++) {
          DrawChar(renderer, fontTexture, mode_str[j],
                   sx + offset * CHAR_WIDTH, status_y);
          offset++;
        }
        for (int d = 0; d < diag_count; d++) {
          if (diagnostics[d].line == cursor_row) {
            char diag[512];
            snprintf(diag, sizeof(diag), "  |  %s: %s",
                     diagnostics[d].severity == 1 ? "ERR" : "WARN",
                     diagnostics[d].message);
            if (diagnostics[d].severity == 1) {
              SDL_SetTextureColorMod(fontTexture, 235, 111, 146);
            } else {
              SDL_SetTextureColorMod(fontTexture, 246, 193, 119);
            }
            for (int j = 0; diag[j]; j++) {
              DrawChar(renderer, fontTexture, diag[j],
                       sx + offset * CHAR_WIDTH, status_y);
              offset++;
            }
            break;
          }
        }
      }
    }

    SDL_SetTextureColorMod(fontTexture, 224, 222, 244);

    /* ── Completion popup ───────────────────────── */
    if (completion_count > 0 && user.state == INSERT) {
      int popup_x = GUTTER_WIDTH + cursor_col * CHAR_WIDTH;
      int popup_y = cursor.y + CHAR_HEIGHT;
      int popup_w = 40 * CHAR_WIDTH;
      int max_visible = 8;
      int vis = completion_count < max_visible ? completion_count : max_visible;
      int popup_h = vis * CHAR_HEIGHT;
      if (popup_y + popup_h > text_area_height)
        popup_y = cursor.y - popup_h;
      SDL_SetRenderDrawColor(renderer, 38, 35, 58, 255);
      SDL_FRect bg = {popup_x, popup_y, popup_w, popup_h};
      SDL_RenderFillRect(renderer, &bg);
      SDL_SetRenderDrawColor(renderer, 82, 79, 103, 255);
      SDL_RenderRect(renderer, &bg);

      int start_idx = 0;
      if (completion_selected >= max_visible)
        start_idx = completion_selected - max_visible + 1;

      for (int i = 0; i < vis; i++) {
        int idx = start_idx + i;
        if (idx >= completion_count) break;
        int y_pos = popup_y + i * CHAR_HEIGHT;
        if (idx == completion_selected) {
          SDL_SetRenderDrawColor(renderer, 64, 61, 82, 255);
          SDL_FRect sel = {popup_x, y_pos, popup_w, CHAR_HEIGHT};
          SDL_RenderFillRect(renderer, &sel);
        }
        SDL_SetTextureColorMod(fontTexture, 224, 222, 244);
        for (int j = 0; completions[idx].label[j]; j++) {
          DrawChar(renderer, fontTexture, completions[idx].label[j],
                   popup_x + j * CHAR_WIDTH, y_pos);
        }
        if (completions[idx].detail[0]) {
          SDL_SetTextureColorMod(fontTexture, 144, 140, 170);
          int det_x = popup_x + (30 * CHAR_WIDTH);
          for (int j = 0; completions[idx].detail[j]; j++) {
            DrawChar(renderer, fontTexture, completions[idx].detail[j],
                     det_x + j * CHAR_WIDTH, y_pos);
          }
        }
      }
      SDL_SetTextureColorMod(fontTexture, 224, 222, 244);
    }

    /* ── Present ────────────────────────────────── */
    SDL_RenderPresent(renderer);
  }
  if (lsp) {
    lsp_close(lsp);
    lsp_destroy(lsp);
  }
  buffer_destroy(&buf);
  TTF_CloseFont(font);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}
