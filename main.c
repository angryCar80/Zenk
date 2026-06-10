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
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define UNDO_MAX 100

typedef struct {
  int line;
  int severity;
  char message[256];
} Diag;

Diag diagnostics[256];
int diag_count = 0;
int is_makefile = 0;
char git_branch[64] = "";

typedef struct {
  char label[256];
  char insert[256];
  char detail[256];
} CompletionItem;
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
  if (len == 0)
    return 0;
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
  if (len == 0)
    return 0;
  for (int r = start_row; r >= 0; r--) {
    int end = (r == start_row) ? start_col : (int)strlen(buf->lines[r]);
    char *last = NULL;
    char *p = buf->lines[r];
    while ((p = strstr(p, pattern)) != NULL) {
      int off = (int)(p - buf->lines[r]);
      if (off < end) {
        last = p;
        p++;
      } else
        break;
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
      if (off < end) {
        last = p;
        p++;
      } else
        break;
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

  // Makefile highlighting
  if (is_makefile) {
    if (c == '#')
      return 2;
    if (c == '$')
      return 4;
    if (col == 0 && is_word_char(c)) {
      for (int i = 0; line[i]; i++) {
        if (line[i] == '#')
          break;
        if (line[i] == ':')
          return 1;
      }
    }
  }

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
  while (col > 0 && is_word_char(line[col - 1]))
    col--;
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
  case '(':
  case ')':
    open = '(';
    close = ')';
    break;
  case '{':
  case '}':
    open = '{';
    close = '}';
    break;
  case '[':
  case ']':
    open = '[';
    close = ']';
    break;
  case '"':
    open = '"';
    close = '"';
    break;
  case '\'':
    open = '\'';
    close = '\'';
    break;
  default:
    return;
  }
  int open_pos = -1, depth = 0;
  for (int i = *col - 1; i >= 0; i--) {
    if (open == close) {
      if (buf->lines[*row][i] == open) {
        open_pos = i;
        break;
      }
    } else {
      if (buf->lines[*row][i] == close)
        depth++;
      else if (buf->lines[*row][i] == open) {
        if (depth == 0) {
          open_pos = i;
          break;
        }
        depth--;
      }
    }
  }
  if (open_pos < 0)
    return;
  int close_pos = -1;
  depth = 0;
  for (int i = open_pos + 1; buf->lines[*row][i]; i++) {
    if (open == close) {
      if (buf->lines[*row][i] == close) {
        close_pos = i;
        break;
      }
    } else {
      if (buf->lines[*row][i] == open)
        depth++;
      else if (buf->lines[*row][i] == close) {
        if (depth == 0) {
          close_pos = i;
          break;
        }
        depth--;
      }
    }
  }
  if (close_pos < 0)
    return;
  int count = close_pos - open_pos - 1;
  for (int i = 0; i < count; i++)
    buffer_delete_char(buf, *row, open_pos + 2);
  *col = open_pos + 1;
  if (*col < 0)
    *col = 0;
}

static int entry_cmp(const void *a, const void *b) {
  return strcmp(*(const char **)a, *(const char **)b);
}

void load_directory(Buffer *buf, const char *path) {
  DIR *d = opendir(path);
  if (!d)
    return;
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
  for (int i = 0; i < count; i++)
    all[i + 1] = entries[i];
  free(entries);
  for (int i = 0; i < buf->line_count; i++)
    free(buf->lines[i]);
  free(buf->lines);
  buf->capacity = total;
  buf->lines = all;
  buf->line_count = total;
}

int name_is_makefile(const char *path) {
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  return strcmp(name, "Makefile") == 0 || strcmp(name, "makefile") == 0 ||
         strcmp(name, "GNUmakefile") == 0 ||
         (strlen(name) > 3 && strcmp(name + strlen(name) - 3, ".mk") == 0);
}

void update_git_info(void) {
  git_branch[0] = '\0';
  FILE *f = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
  if (f) {
    if (fgets(git_branch, sizeof(git_branch), f)) {
      int len = strlen(git_branch);
      if (len > 0 && git_branch[len - 1] == '\n')
        git_branch[len - 1] = '\0';
    }
    pclose(f);
  }
}

int main(int argc, char *argv[]) {
  Cursor cursor;
  Buffer buf_store[8];
  char fname_store[8][512];
  int buf_count = 0;
  int cur_buf = 0;
  if (argc > 1) {
    for (int i = 1; i < argc && buf_count < 8; i++) {
      strncpy(fname_store[buf_count], argv[i],
              sizeof(fname_store[buf_count]) - 1);
      buf_store[buf_count] = buffer_load(fname_store[buf_count]);
      buf_count++;
    }
  }
  if (buf_count == 0) {
    buf_store[0] = buffer_create(300);
    buf_count = 1;
  }
  LspClient *lsp = NULL;
  if (fname_store[cur_buf][0]) {
    lsp = lsp_init(fname_store[cur_buf]);
    if (lsp)
      lsp_open(lsp, &buf_store[cur_buf]);
    is_makefile = name_is_makefile(fname_store[cur_buf]);
    update_git_info();
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

  TTF_Font *font = TTF_OpenFont(
      "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf", 16);

  if (!font) {
    printf("Font Loading Faild\n");
    return 1;
  }

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
  int pending_replace = 0;
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
  int showing_make = 0;
  char make_saved_filename[512] = "";
  char last_search[256] = "";
  char status_msg[512] = "";
  Uint64 status_msg_time = 0;
  // Main Loop
  while (running) {
    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    int TAB_BAR_HEIGHT = CHAR_HEIGHT + 8;
    int text_area_height = h - TAB_BAR_HEIGHT - CHAR_HEIGHT;
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
              buffer_insert_char(&buf_store[cur_buf], cursor_row, cursor_col,
                                 c);
              cursor_col++;
              buffer_insert_char(&buf_store[cur_buf], cursor_row, cursor_col,
                                 close);
              cursor_col--;
              if (lsp)
                lsp_change(lsp, &buf_store[cur_buf]);
            } else if (c == '"' || c == '\'') {
              buffer_insert_char(&buf_store[cur_buf], cursor_row, cursor_col,
                                 c);
              cursor_col++;
              buffer_insert_char(&buf_store[cur_buf], cursor_row, cursor_col,
                                 c);
              cursor_col--;
              if (lsp)
                lsp_change(lsp, &buf_store[cur_buf]);
              if (base_count > 0)
                filter_completions(buf_store[cur_buf].lines[cursor_row],
                                   cursor_col);
            } else if (c >= 32) {
              buffer_insert_char(&buf_store[cur_buf], cursor_row, cursor_col,
                                 c);
              cursor_col++;
              if (lsp)
                lsp_change(lsp, &buf_store[cur_buf]);
              if (base_count > 0)
                filter_completions(buf_store[cur_buf].lines[cursor_row],
                                   cursor_col);
              if (lsp && completion_count == 0 &&
                  SDL_GetTicks() - last_completion_time > 120) {
                lsp_request_completion(lsp, cursor_row, cursor_col);
                last_completion_time = SDL_GetTicks();
              }
            }
          }
        } else if (user.state == NORMAL && pending_motion == 'i' &&
                   strchr("(){}[]\"'", event.text.text[0])) {
          push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
          delete_inside(&buf_store[cur_buf], &cursor_row, &cursor_col,
                        event.text.text[0]);
          cursor_col_target = cursor_col;
          pending_motion = 0;
          if (lsp)
            lsp_change(lsp, &buf_store[cur_buf]);
        } else if (user.state == NORMAL && pending_replace) {
          push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
          int line_len = strlen(buf_store[cur_buf].lines[cursor_row]);
          if (cursor_col < line_len && event.text.text[0] >= 32) {
            buf_store[cur_buf].lines[cursor_row][cursor_col] =
                event.text.text[0];
            if (lsp)
              lsp_change(lsp, &buf_store[cur_buf]);
          }
          pending_replace = 0;
          swallow_until = SDL_GetTicks() + 50;
        } else if (user.state == NORMAL && event.text.text[0] == '$') {
          cursor_col = strlen(buf_store[cur_buf].lines[cursor_row]);
          cursor_col_target = cursor_col;
        } else if (user.state == NORMAL && event.text.text[0] == '^') {
          cursor_col = 0;
          while (buf_store[cur_buf].lines[cursor_row][cursor_col] == ' ')
            cursor_col++;
          cursor_col_target = cursor_col;
        } else if (user.state == NORMAL && event.text.text[0] == '%') {
          char c = buf_store[cur_buf].lines[cursor_row][cursor_col];
          char open, close;
          int dir;
          if (c == '(') {
            open = '(';
            close = ')';
            dir = 1;
          } else if (c == ')') {
            open = '(';
            close = ')';
            dir = -1;
          } else if (c == '{') {
            open = '{';
            close = '}';
            dir = 1;
          } else if (c == '}') {
            open = '{';
            close = '}';
            dir = -1;
          } else if (c == '[') {
            open = '[';
            close = ']';
            dir = 1;
          } else if (c == ']') {
            open = '[';
            close = ']';
            dir = -1;
          } else {
            open = 0;
            close = 0;
            dir = 0;
          }
          if (dir != 0) {
            int depth = 0;
            int r = cursor_row, co = cursor_col;
            while (r >= 0 && r < buf_store[cur_buf].line_count) {
              char *line = buf_store[cur_buf].lines[r];
              int start = (r == cursor_row)
                              ? co + dir
                              : (dir > 0 ? 0 : (int)strlen(line) - 1);
              int end = (dir > 0) ? (int)strlen(line) : -1;
              for (int ci = start; ci != end; ci += dir) {
                if (dir > 0) {
                  if (line[ci] == open)
                    depth++;
                  if (line[ci] == close)
                    depth--;
                } else {
                  if (line[ci] == close)
                    depth++;
                  if (line[ci] == open)
                    depth--;
                }
                if (depth == 0) {
                  cursor_row = r;
                  cursor_col = ci;
                  cursor_col_target = ci;
                  goto found_match;
                }
              }
              r += dir;
            }
          found_match:;
          }
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
        if (col < 0)
          col = 0;
        int row = (my - TAB_BAR_HEIGHT) / CHAR_HEIGHT + scroll_row;
        if (row >= buf_store[cur_buf].line_count)
          row = buf_store[cur_buf].line_count - 1;
        if (row < 0)
          row = 0;
        int line_len = (int)strlen(buf_store[cur_buf].lines[row]);
        if (col > line_len)
          col = line_len;
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
          if (cursor_col < (int)strlen(buf_store[cur_buf].lines[cursor_row])) {
            cursor_col++;
          }
          cursor_col_target = cursor_col;
        }
        if (event.key.key == SDLK_DOWN) {
          if (cursor_row != buf_store[cur_buf].line_count - 1) {
            cursor_row++;
          }
          int line_len = strlen(buf_store[cur_buf].lines[cursor_row]);
          cursor_col = min(cursor_col_target, line_len);
        }
        if (event.key.key == SDLK_UP) {
          if (cursor_row != 0) {
            cursor_row--;
          }
          int line_len = strlen(buf_store[cur_buf].lines[cursor_row]);
          cursor_col = min(cursor_col_target, line_len);
        }

        if (event.key.key == SDLK_HOME) {
          cursor_col = 0;
          cursor_col_target = 0;
        }
        if (event.key.key == SDLK_END) {
          cursor_col = strlen(buf_store[cur_buf].lines[cursor_row]);
        }
        if (event.key.key == SDLK_PAGEUP) {
          scroll_row -= max_visible_lines;
          if (scroll_row < 0)
            scroll_row = 0;
          cursor_row = scroll_row;
          if (cursor_row < 0)
            cursor_row = 0;
          cursor_col_target = cursor_col;
        }
        if (event.key.key == SDLK_PAGEDOWN) {
          scroll_row += max_visible_lines;
          if (scroll_row + max_visible_lines > buf_store[cur_buf].line_count)
            scroll_row = buf_store[cur_buf].line_count - max_visible_lines;
          if (scroll_row < 0)
            scroll_row = 0;
          cursor_row = scroll_row;
          if (cursor_row >= buf_store[cur_buf].line_count)
            cursor_row = buf_store[cur_buf].line_count - 1;
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
            if (cursor_col <
                (int)strlen(buf_store[cur_buf].lines[cursor_row])) {
              cursor_col++;
            }
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_K && !(event.key.mod & SDL_KMOD_SHIFT)) {
            if (cursor_row != 0) {
              cursor_row--;
            }
            int line_len = strlen(buf_store[cur_buf].lines[cursor_row]);
            cursor_col = min(cursor_col_target, line_len);
          }
          if (event.key.key == SDLK_J && !(event.key.mod & SDL_KMOD_SHIFT)) {
            int count = prefix_count > 0 ? prefix_count : 1;
            for (int c = 0; c < count; c++) {
              if (cursor_row != buf_store[cur_buf].line_count - 1) {
                cursor_row++;
              }
            }
            int line_len = strlen(buf_store[cur_buf].lines[cursor_row]);
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
            undo(&buf_store[cur_buf], &cursor_row, &cursor_col);
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_R && (event.key.mod & SDL_KMOD_CTRL)) {
            redo(&buf_store[cur_buf], &cursor_row, &cursor_col);
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_W) {
            int line_len = strlen(buf_store[cur_buf].lines[cursor_row]);
            int pos = cursor_col;

            while (pos < line_len) {
              char c = buf_store[cur_buf].lines[cursor_row][pos];
              if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_') {
                pos++;
              } else {
                break;
              }
            }
            while (pos < line_len) {
              char c = buf_store[cur_buf].lines[cursor_row][pos];
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
            } else if (cursor_row < buf_store[cur_buf].line_count - 1) {
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
                char c = buf_store[cur_buf].lines[cursor_row][pos];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_'))
                  pos--;
                else
                  break;
              }

              // Skip word chars backward to find start of word
              while (pos > 0) {
                char c = buf_store[cur_buf].lines[cursor_row][pos - 1];
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
              push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
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
            push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
            if (cursor_col > 0) {
              buffer_delete_char(&buf_store[cur_buf], cursor_row, cursor_col);
              cursor_col--;
              lsp_change(lsp, &buf_store[cur_buf]);
            }
          } else if (event.key.key == SDLK_X) {
            // x — delete character forward
            push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
            int count = prefix_count > 0 ? prefix_count : 1;
            for (int c = 0; c < count; c++) {
              int line_len = strlen(buf_store[cur_buf].lines[cursor_row]);
              if (cursor_col >= line_len)
                break;
              buffer_delete_char(&buf_store[cur_buf], cursor_row,
                                 cursor_col + 1);
            }
            prefix_count = 0;
            lsp_change(lsp, &buf_store[cur_buf]);
          }
        }
        if (event.key.key == SDLK_D) {
          if (pending_operator == 'd') {
            push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
            int count = prefix_count > 0 ? prefix_count : 1;
            for (int c = 0; c < count; c++) {
              if (buf_store[cur_buf].line_count <= 1)
                break;
              buffer_delete_line(&buf_store[cur_buf], cursor_row);
              if (cursor_row >= buf_store[cur_buf].line_count)
                cursor_row = buf_store[cur_buf].line_count - 1;
            }
            cursor_col = 0;
            if (lsp)
              lsp_change(lsp, &buf_store[cur_buf]);
            pending_operator = 0;
            prefix_count = 0;
          } else {
            pending_operator = 'd';
          }
        } else if (event.key.key == SDLK_Y) {
          if (pending_operator == 'y') {
            free(yank_buffer);
            yank_buffer =
                malloc(strlen(buf_store[cur_buf].lines[cursor_row]) + 1);
            strcpy(yank_buffer, buf_store[cur_buf].lines[cursor_row]);
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
              push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
              buffer_insert_line(&buf_store[cur_buf], cursor_row + 1);
              free(buf_store[cur_buf].lines[cursor_row + 1]);
              buf_store[cur_buf].lines[cursor_row + 1] =
                  malloc(strlen(yank_buffer) + 1);
              strcpy(buf_store[cur_buf].lines[cursor_row + 1], yank_buffer);
              cursor_row++;
              cursor_col = 0;
              if (lsp) {
                lsp_change(lsp, &buf_store[cur_buf]);
              }
            }
          }
          if (event.key.key == SDLK_O && (event.key.mod & SDL_KMOD_SHIFT)) {
            // O — open line ABOVE
            push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
            buffer_insert_line(&buf_store[cur_buf], cursor_row);
            user.state = INSERT;
            cursor_col = 0;
            swallow_until = SDL_GetTicks() + 50;
            if (lsp)
              lsp_change(lsp, &buf_store[cur_buf]);
          } else if (event.key.key == SDLK_O) {
            push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
            buffer_insert_line(&buf_store[cur_buf], cursor_row + 1);
            user.state = INSERT;
            cursor_row++;
            cursor_col = 0;
            swallow_until = SDL_GetTicks() + 50;
            if (lsp)
              lsp_change(lsp, &buf_store[cur_buf]);
          }
          if (event.key.key == SDLK_A && (event.key.mod & SDL_KMOD_SHIFT)) {
            // A — append at end of line
            push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
            cursor_col = strlen(buf_store[cur_buf].lines[cursor_row]);
            user.state = INSERT;
            swallow_until = SDL_GetTicks() + 50;
          } else if (event.key.key == SDLK_A) {
            // a — append after cursor
            push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
            if (cursor_col < (int)strlen(buf_store[cur_buf].lines[cursor_row]))
              cursor_col++;
            user.state = INSERT;
            swallow_until = SDL_GetTicks() + 50;
          }
          // r — replace char under cursor
          if (event.key.key == SDLK_R && !(event.key.mod & SDL_KMOD_SHIFT)) {
            pending_replace = 1;
            swallow_until = SDL_GetTicks() + 50;
          }
          // K — hover info
          if (event.key.key == SDLK_K && (event.key.mod & SDL_KMOD_SHIFT)) {
            if (lsp)
              lsp_request_hover(lsp, cursor_row, cursor_col);
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
            if (lsp)
              lsp_request_definition(lsp, cursor_row, cursor_col);
          } else {
            pending_g = 0;
          }
          // n / N — search next/prev
          if (event.key.key == SDLK_N && last_search[0]) {
            int nr, nc;
            int shift = (event.key.mod & SDL_KMOD_SHIFT);
            int found =
                shift ? search_backward(&buf_store[cur_buf], last_search,
                                        cursor_row, cursor_col, &nr, &nc)
                      : search_forward(&buf_store[cur_buf], last_search,
                                       cursor_row, cursor_col + 1, &nr, &nc);
            if (found) {
              cursor_row = nr;
              cursor_col = nc;
              cursor_col_target = nc;
              snprintf(status_msg, sizeof(status_msg), "/%s", last_search);
              status_msg_time = SDL_GetTicks();
            } else {
              snprintf(status_msg, sizeof(status_msg), "Pattern not found: %s",
                       last_search);
              status_msg_time = SDL_GetTicks();
            }
          }
          // Ctrl+D — half page down, Ctrl+U — half page up
          if (event.key.key == SDLK_D && (event.key.mod & SDL_KMOD_CTRL)) {
            int half = max_visible_lines / 2;
            cursor_row += half;
            if (cursor_row >= buf_store[cur_buf].line_count)
              cursor_row = buf_store[cur_buf].line_count - 1;
            cursor_col_target = cursor_col;
          }
          if (event.key.key == SDLK_U && (event.key.mod & SDL_KMOD_CTRL)) {
            int half = max_visible_lines / 2;
            cursor_row -= half;
            if (cursor_row < 0)
              cursor_row = 0;
            cursor_col_target = cursor_col;
          }
          // G — go to last line (or line N with prefix)
          if (event.key.key == SDLK_G && (event.key.mod & SDL_KMOD_SHIFT)) {
            if (prefix_count > 0) {
              cursor_row = prefix_count - 1;
              if (cursor_row >= buf_store[cur_buf].line_count)
                cursor_row = buf_store[cur_buf].line_count - 1;
              prefix_count = 0;
            } else {
              cursor_row = buf_store[cur_buf].line_count - 1;
            }
            cursor_col = 0;
            cursor_col_target = 0;
          }
          // J — join next line
          if (event.key.key == SDLK_J && (event.key.mod & SDL_KMOD_SHIFT)) {
            if (cursor_row < buf_store[cur_buf].line_count - 1) {
              push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
              int cur_len = strlen(buf_store[cur_buf].lines[cursor_row]);
              int next_len = strlen(buf_store[cur_buf].lines[cursor_row + 1]);
              buf_store[cur_buf].lines[cursor_row] = realloc(
                  buf_store[cur_buf].lines[cursor_row], cur_len + next_len + 1);
              memcpy(buf_store[cur_buf].lines[cursor_row] + cur_len,
                     buf_store[cur_buf].lines[cursor_row + 1], next_len + 1);
              buffer_delete_line(&buf_store[cur_buf], cursor_row + 1);
              lsp_change(lsp, &buf_store[cur_buf]);
            }
          }
          // Tab — next buffer, Shift+Tab — prev buffer
          if (event.key.key == SDLK_TAB && !event.key.repeat) {
            if (event.key.mod & SDL_KMOD_SHIFT) {
              if (buf_count > 1) {
                cur_buf = (cur_buf - 1 + buf_count) % buf_count;
                if (lsp) {
                  lsp_close(lsp);
                  lsp_destroy(lsp);
                  lsp = NULL;
                }
                if (fname_store[cur_buf][0]) {
                  lsp = lsp_init(fname_store[cur_buf]);
                  if (lsp)
                    lsp_open(lsp, &buf_store[cur_buf]);
                  is_makefile = name_is_makefile(fname_store[cur_buf]);
                }
                update_git_info();
                cursor_row = 0;
                cursor_col = 0;
                scroll_row = 0;
              }
            } else if (buf_count > 1) {
              cur_buf = (cur_buf + 1) % buf_count;
              if (lsp) {
                lsp_close(lsp);
                lsp_destroy(lsp);
                lsp = NULL;
              }
              if (fname_store[cur_buf][0]) {
                lsp = lsp_init(fname_store[cur_buf]);
                if (lsp)
                  lsp_open(lsp, &buf_store[cur_buf]);
                is_makefile = name_is_makefile(fname_store[cur_buf]);
              }
              update_git_info();
              cursor_row = 0;
              cursor_col = 0;
              scroll_row = 0;
            }
          }
        }
      }
      // Explorer mode: Enter opens file/dir, - goes up
      if (exploring && user.state == NORMAL) {
        if (event.key.key == SDLK_RETURN && !event.key.repeat &&
            SDL_GetTicks() - last_enter_time > 100) {
          last_enter_time = SDL_GetTicks();
          char *line = buf_store[cur_buf].lines[cursor_row];
          int len = strlen(line);
          if (len > 0 && line[len - 1] == '/') {
            char new_path[1024];
            line[len - 1] = '\0';
            snprintf(new_path, sizeof(new_path), "%s/%s", explore_dir, line);
            line[len - 1] = '/';
            strncpy(explore_dir, new_path, sizeof(explore_dir) - 1);
            load_directory(&buf_store[cur_buf], explore_dir);
            cursor_row = 0;
            cursor_col = 0;
          } else {
            char full_path[1024];
            // Trim trailing space from file entries
            int flen = len;
            while (flen > 0 && line[flen - 1] == ' ')
              flen--;
            snprintf(full_path, sizeof(full_path), "%s/%.*s", explore_dir, flen,
                     line);
            if (lsp) {
              lsp_close(lsp);
              lsp_destroy(lsp);
              lsp = NULL;
            }
            buffer_destroy(&buf_store[cur_buf]);
            buf_store[cur_buf] = buffer_load(full_path);
            strncpy(fname_store[cur_buf], full_path,
                    sizeof(fname_store[cur_buf]) - 1);
            lsp = lsp_init(fname_store[cur_buf]);
            if (lsp)
              lsp_open(lsp, &buf_store[cur_buf]);
            is_makefile = name_is_makefile(fname_store[cur_buf]);
            update_git_info();
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
          load_directory(&buf_store[cur_buf], explore_dir);
          cursor_row = 0;
          cursor_col = 0;
        }
      }
      // Make output: Enter on file:line:col opens the file
      if (showing_make && user.state == NORMAL &&
          event.key.key == SDLK_RETURN && !event.key.repeat) {
        char *line = buf_store[cur_buf].lines[cursor_row];
        int fname_len = 0, lineno = 0;
        if (sscanf(line, "%*[^:]:%d:%n", &lineno, &fname_len) >= 1 &&
            fname_len > 0) {
          char fname[512];
          sscanf(line, "%511s", fname);
          // Remove trailing colon if present
          int fl = strlen(fname);
          if (fl > 0 && fname[fl - 1] == ':')
            fname[fl - 1] = '\0';
          char resolve[1024];
          const char *dir = make_saved_filename[0] ? make_saved_filename : ".";
          const char *last_slash = strrchr(dir, '/');
          if (last_slash) {
            int dlen = last_slash - dir;
            snprintf(resolve, sizeof(resolve), "%.*s/%s", dlen, dir, fname);
          } else {
            snprintf(resolve, sizeof(resolve), "%s", fname);
          }
          buffer_destroy(&buf_store[cur_buf]);
          buf_store[cur_buf] = buffer_load(resolve);
          strncpy(fname_store[cur_buf], resolve,
                  sizeof(fname_store[cur_buf]) - 1);
          if (lsp) {
            lsp_close(lsp);
            lsp_destroy(lsp);
            lsp = NULL;
          }
          lsp = lsp_init(fname_store[cur_buf]);
          if (lsp)
            lsp_open(lsp, &buf_store[cur_buf]);
          is_makefile = name_is_makefile(fname_store[cur_buf]);
          update_git_info();
          showing_make = 0;
          cursor_row = lineno > 0 ? lineno - 1 : 0;
          cursor_col = 0;
          cursor_col_target = 0;
          scroll_row = 0;
          if (cursor_row >= buf_store[cur_buf].line_count)
            cursor_row = buf_store[cur_buf].line_count - 1;
        }
        last_enter_time = SDL_GetTicks();
      }
      if (user.state == INSERT) {
        if (completion_count > 0 && event.key.key == SDLK_DOWN &&
            SDL_GetTicks() - last_comp_nav_time > 100) {
          last_comp_nav_time = SDL_GetTicks();
          completion_selected = (completion_selected + 1) % completion_count;
        } else if (completion_count > 0 && event.key.key == SDLK_TAB &&
                   !(event.key.mod & SDL_KMOD_SHIFT) &&
                   SDL_GetTicks() - last_comp_nav_time > 100) {
          last_comp_nav_time = SDL_GetTicks();
          completion_selected = (completion_selected + 1) % completion_count;
        } else if (completion_count > 0 &&
                   ((event.key.key == SDLK_TAB &&
                     (event.key.mod & SDL_KMOD_SHIFT)) ||
                    event.key.key == SDLK_UP) &&
                   SDL_GetTicks() - last_comp_nav_time > 100) {
          last_comp_nav_time = SDL_GetTicks();
          completion_selected =
              (completion_selected - 1 + completion_count) % completion_count;
        } else if (completion_count > 0 && event.key.key == SDLK_RETURN &&
                   !event.key.repeat) {
          last_enter_time = SDL_GetTicks();
          CompletionItem *ci = &completions[completion_selected];
          char *ins = ci->insert;
          int ws = get_word_prefix_start(buf_store[cur_buf].lines[cursor_row],
                                         cursor_col);
          for (int i = ws; i < cursor_col; i++)
            buffer_delete_char(&buf_store[cur_buf], cursor_row, ws + 1);
          cursor_col = ws;
          for (int i = 0; ins[i]; i++) {
            buffer_insert_char(&buf_store[cur_buf], cursor_row, cursor_col,
                               ins[i]);
            cursor_col++;
          }
          if (lsp)
            lsp_change(lsp, &buf_store[cur_buf]);
          base_count = 0;
          completion_count = 0;
        } else if (completion_count > 0 && event.key.key == SDLK_ESCAPE) {
          base_count = 0;
          completion_count = 0;
        } else if (event.key.key == SDLK_TAB &&
                   SDL_GetTicks() - last_tab_time > 100) {
          last_tab_time = SDL_GetTicks();
          buffer_insert_char(&buf_store[cur_buf], cursor_row, cursor_col, ' ');
          cursor_col++;
          buffer_insert_char(&buf_store[cur_buf], cursor_row, cursor_col, ' ');
          cursor_col++;
          swallow_until = SDL_GetTicks() + 50;
          if (lsp) {
            lsp_change(lsp, &buf_store[cur_buf]);
          }
        } else if (event.key.key == SDLK_RETURN && !event.key.repeat &&
                   completion_count == 0 &&
                   SDL_GetTicks() - last_enter_time > 100) {
          last_enter_time = SDL_GetTicks();
          int len = strlen(buf_store[cur_buf].lines[cursor_row]);

          int indent = 0;
          while (buf_store[cur_buf].lines[cursor_row][indent] == ' ' ||
                 buf_store[cur_buf].lines[cursor_row][indent] == '\t') {
            indent++;
          }
          int tail_len = len - cursor_col;
          buffer_insert_line(&buf_store[cur_buf], cursor_row + 1);

          char *new_line = malloc(indent + tail_len + 1);
          memcpy(new_line, buf_store[cur_buf].lines[cursor_row], indent);
          memcpy(new_line + indent,
                 buf_store[cur_buf].lines[cursor_row] + cursor_col, tail_len);
          new_line[indent + tail_len] = '\0';
          buf_store[cur_buf].lines[cursor_row + 1] = new_line;

          buf_store[cur_buf].lines[cursor_row] =
              realloc(buf_store[cur_buf].lines[cursor_row], cursor_col + 1);
          buf_store[cur_buf].lines[cursor_row][cursor_col] = '\0';

          cursor_row++;
          cursor_col = indent;
          swallow_until = SDL_GetTicks() + 50;
          if (lsp) {
            lsp_change(lsp, &buf_store[cur_buf]);
          }
        } else if (event.key.key == SDLK_BACKSPACE && !event.key.repeat &&
                   SDL_GetTicks() - last_bksp_time > 100) {
          last_bksp_time = SDL_GetTicks();
          if (cursor_col > 0) {
            // Empty auto-pair: delete both brackets
            if (cursor_col <
                    (int)strlen(buf_store[cur_buf].lines[cursor_row]) &&
                ((buf_store[cur_buf].lines[cursor_row][cursor_col - 1] == '(' &&
                  buf_store[cur_buf].lines[cursor_row][cursor_col] == ')') ||
                 (buf_store[cur_buf].lines[cursor_row][cursor_col - 1] == '{' &&
                  buf_store[cur_buf].lines[cursor_row][cursor_col] == '}') ||
                 (buf_store[cur_buf].lines[cursor_row][cursor_col - 1] == '[' &&
                  buf_store[cur_buf].lines[cursor_row][cursor_col] == ']') ||
                 (buf_store[cur_buf].lines[cursor_row][cursor_col - 1] == '"' &&
                  buf_store[cur_buf].lines[cursor_row][cursor_col] == '"') ||
                 (buf_store[cur_buf].lines[cursor_row][cursor_col - 1] ==
                      '\'' &&
                  buf_store[cur_buf].lines[cursor_row][cursor_col] == '\''))) {
              buffer_delete_char(&buf_store[cur_buf], cursor_row,
                                 cursor_col + 1);
              cursor_col--;
              buffer_delete_char(&buf_store[cur_buf], cursor_row,
                                 cursor_col + 1);
              if (lsp)
                lsp_change(lsp, &buf_store[cur_buf]);
            } else {
              buffer_delete_char(&buf_store[cur_buf], cursor_row, cursor_col);
              cursor_col--;
              if (lsp)
                lsp_change(lsp, &buf_store[cur_buf]);
            }
          } else if (cursor_row > 0) {
            int prev_len = strlen(buf_store[cur_buf].lines[cursor_row - 1]);
            int cur_len = strlen(buf_store[cur_buf].lines[cursor_row]);
            buf_store[cur_buf].lines[cursor_row - 1] =
                realloc(buf_store[cur_buf].lines[cursor_row - 1],
                        prev_len + cur_len + 1);
            memcpy(buf_store[cur_buf].lines[cursor_row - 1] + prev_len,
                   buf_store[cur_buf].lines[cursor_row], cur_len + 1);
            buffer_delete_line(&buf_store[cur_buf], cursor_row);
            cursor_row--;
            cursor_col = prev_len;
            if (lsp)
              lsp_change(lsp, &buf_store[cur_buf]);
          }
          if (base_count > 0)
            filter_completions(buf_store[cur_buf].lines[cursor_row],
                               cursor_col);
        } else if (event.key.key == SDLK_DELETE && !event.key.repeat) {
          // Del — delete forward
          int line_len = strlen(buf_store[cur_buf].lines[cursor_row]);
          if (cursor_col < line_len) {
            buffer_delete_char(&buf_store[cur_buf], cursor_row, cursor_col + 1);
            if (lsp)
              lsp_change(lsp, &buf_store[cur_buf]);
          } else if (cursor_row < buf_store[cur_buf].line_count - 1) {
            int cur_len = strlen(buf_store[cur_buf].lines[cursor_row]);
            int next_len = strlen(buf_store[cur_buf].lines[cursor_row + 1]);
            buf_store[cur_buf].lines[cursor_row] = realloc(
                buf_store[cur_buf].lines[cursor_row], cur_len + next_len + 1);
            memcpy(buf_store[cur_buf].lines[cursor_row] + cur_len,
                   buf_store[cur_buf].lines[cursor_row + 1], next_len + 1);
            buffer_delete_line(&buf_store[cur_buf], cursor_row + 1);
            if (lsp)
              lsp_change(lsp, &buf_store[cur_buf]);
          }
        } else if (event.key.key == SDLK_W && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+w — delete word backward
          int ws = cursor_col;
          while (ws > 0 && buf_store[cur_buf].lines[cursor_row][ws - 1] == ' ')
            ws--;
          while (ws > 0 &&
                 is_word_char(buf_store[cur_buf].lines[cursor_row][ws - 1]))
            ws--;
          for (int i = ws; i < cursor_col; i++)
            buffer_delete_char(&buf_store[cur_buf], cursor_row, ws + 1);
          cursor_col = ws;
          swallow_until = SDL_GetTicks() + 50;
          if (lsp)
            lsp_change(lsp, &buf_store[cur_buf]);
        } else if (event.key.key == SDLK_U && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+u — delete to start of line
          for (int i = 0; i < cursor_col; i++)
            buffer_delete_char(&buf_store[cur_buf], cursor_row, 1);
          cursor_col = 0;
          swallow_until = SDL_GetTicks() + 50;
          if (lsp)
            lsp_change(lsp, &buf_store[cur_buf]);
        } else if (event.key.key == SDLK_S && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+s — save
          buffer_save(&buf_store[cur_buf], fname_store[cur_buf]);
          save_feedback_time = SDL_GetTicks();
          swallow_until = SDL_GetTicks() + 50;
        } else if (event.key.key == SDLK_C && (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+c — exit to NORMAL
          user.state = NORMAL;
          swallow_until = SDL_GetTicks() + 50;
        } else if (event.key.key == SDLK_SPACE &&
                   (event.key.mod & SDL_KMOD_CTRL)) {
          // Ctrl+Space — manual completion trigger
          if (lsp) {
            lsp_request_completion(lsp, cursor_row, cursor_col);
            last_completion_time = SDL_GetTicks();
          }
        }
      }
      if (user.state == VISUAL) {
        if (event.key.key == SDLK_D || event.key.key == SDLK_X) {
          push_undo(&buf_store[cur_buf], cursor_row, cursor_col);
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
            memcpy(yank_buffer, buf_store[cur_buf].lines[sr] + sc, range);
            yank_buffer[range] = '\0';

            int len = strlen(buf_store[cur_buf].lines[sr]);
            memmove(buf_store[cur_buf].lines[sr] + sc,
                    buf_store[cur_buf].lines[sr] + ec, len - ec + 1);
            buf_store[cur_buf].lines[sr] =
                realloc(buf_store[cur_buf].lines[sr], len - range + 1);
          } else {
            // Multi-line: copy everything into yank_buffer with \n
            int tail_len = strlen(buf_store[cur_buf].lines[sr]) - sc;
            int yank_size = tail_len + 1; // first line tail + \n
            for (int i = sr + 1; i < er; i++)
              yank_size +=
                  strlen(buf_store[cur_buf].lines[i]) + 1; // middle lines + \n
            yank_size += ec + 1; // end row head + null

            yank_buffer = malloc(yank_size);
            int pos = 0;
            memcpy(yank_buffer + pos, buf_store[cur_buf].lines[sr] + sc,
                   tail_len);
            pos += tail_len;
            yank_buffer[pos++] = '\n';
            for (int i = sr + 1; i < er; i++) {
              int len = strlen(buf_store[cur_buf].lines[i]);
              memcpy(yank_buffer + pos, buf_store[cur_buf].lines[i], len);
              pos += len;
              yank_buffer[pos++] = '\n';
            }
            memcpy(yank_buffer + pos, buf_store[cur_buf].lines[er], ec);
            pos += ec;
            yank_buffer[pos] = '\0';

            // Delete: append end row's tail to start row
            int end_tail_len = strlen(buf_store[cur_buf].lines[er]) - ec;
            buf_store[cur_buf].lines[sr] =
                realloc(buf_store[cur_buf].lines[sr], sc + end_tail_len + 1);
            memcpy(buf_store[cur_buf].lines[sr] + sc,
                   buf_store[cur_buf].lines[er] + ec, end_tail_len);
            buf_store[cur_buf].lines[sr][sc + end_tail_len] = '\0';

            // Free all lines from sr+1 to er
            for (int i = sr + 1; i <= er; i++)
              free(buf_store[cur_buf].lines[i]);
            // Shift remaining lines down
            int lines_to_remove = er - sr;
            memmove(&buf_store[cur_buf].lines[sr + 1],
                    &buf_store[cur_buf].lines[er + 1],
                    sizeof(char *) * (buf_store[cur_buf].line_count - er - 1));
            buf_store[cur_buf].line_count -= lines_to_remove;
          }

          cursor_row = sr;
          cursor_col = sc;
          cursor_col_target = sc;
          user.state = NORMAL;
          if (lsp)
            lsp_change(lsp, &buf_store[cur_buf]);
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
            memcpy(yank_buffer, buf_store[cur_buf].lines[sr] + sc, range);
            yank_buffer[range] = '\0';
          } else {
            int tail_len = strlen(buf_store[cur_buf].lines[sr]) - sc;
            int yank_size = tail_len + 1;
            for (int i = sr + 1; i < er; i++)
              yank_size += strlen(buf_store[cur_buf].lines[i]) + 1;
            yank_size += ec + 1;

            yank_buffer = malloc(yank_size);
            int pos = 0;
            memcpy(yank_buffer + pos, buf_store[cur_buf].lines[sr] + sc,
                   tail_len);
            pos += tail_len;
            yank_buffer[pos++] = '\n';
            for (int i = sr + 1; i < er; i++) {
              int len = strlen(buf_store[cur_buf].lines[i]);
              memcpy(yank_buffer + pos, buf_store[cur_buf].lines[i], len);
              pos += len;
              yank_buffer[pos++] = '\n';
            }
            memcpy(yank_buffer + pos, buf_store[cur_buf].lines[er], ec);
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
            if (search_forward(&buf_store[cur_buf], last_search, cursor_row,
                               cursor_col + 1, &nr, &nc)) {
              cursor_row = nr;
              cursor_col = nc;
              cursor_col_target = nc;
              snprintf(status_msg, sizeof(status_msg), "/%s", last_search);
              status_msg_time = SDL_GetTicks();
            } else {
              snprintf(status_msg, sizeof(status_msg), "Pattern not found: %s",
                       last_search);
              status_msg_time = SDL_GetTicks();
            }
          } else if (cmd_buf[0] == '/' && cmd_len == 1 && last_search[0]) {
            // // — repeat last search
            int nr, nc;
            if (search_forward(&buf_store[cur_buf], last_search, cursor_row,
                               cursor_col + 1, &nr, &nc)) {
              cursor_row = nr;
              cursor_col = nc;
              cursor_col_target = nc;
              snprintf(status_msg, sizeof(status_msg), "/%s", last_search);
              status_msg_time = SDL_GetTicks();
            } else {
              snprintf(status_msg, sizeof(status_msg), "Pattern not found: %s",
                       last_search);
              status_msg_time = SDL_GetTicks();
            }
          } else if (strcmp(cmd_buf, ":w") == 0) {
            buffer_save(&buf_store[cur_buf], fname_store[cur_buf]);
            save_feedback_time = SDL_GetTicks();
          } else if (strcmp(cmd_buf, ":q") == 0) {
            if (buf_store[cur_buf].dirty && fname_store[cur_buf][0]) {
              snprintf(status_msg, sizeof(status_msg),
                       "No write since last change (add ! to override)");
              status_msg_time = SDL_GetTicks();
            } else {
              running = false;
            }
          } else if (strcmp(cmd_buf, ":q!") == 0)
            running = false;
          else if (strcmp(cmd_buf, ":wq") == 0 ||
                   strcmp(cmd_buf, ":wq!") == 0) {
            buffer_save(&buf_store[cur_buf], fname_store[cur_buf]);
            save_feedback_time = SDL_GetTicks();
            running = false;
          } else if (strcmp(cmd_buf, ":w!") == 0) {
            buffer_save(&buf_store[cur_buf], fname_store[cur_buf]);
            save_feedback_time = SDL_GetTicks();
          } else if (strcmp(cmd_buf, ":e") == 0) {
            if (buf_count < 8) {
              strncpy(explore_dir, ".", sizeof(explore_dir) - 1);
              exploring = 1;
              if (lsp) {
                lsp_close(lsp);
                lsp_destroy(lsp);
                lsp = NULL;
              }
              cur_buf = buf_count;
              buf_store[cur_buf] = buffer_create(100);
              strncpy(fname_store[cur_buf], explore_dir,
                      sizeof(fname_store[cur_buf]) - 1);
              load_directory(&buf_store[cur_buf], explore_dir);
              buf_count++;
              cursor_row = 0;
              cursor_col = 0;
              cursor_col_target = 0;
              scroll_row = 0;
            } else {
              snprintf(status_msg, sizeof(status_msg), "Max 8 buffers");
              status_msg_time = SDL_GetTicks();
            }
          } else if (strcmp(cmd_buf, ":bn") == 0) {
            if (buf_count > 1) {
              cur_buf = (cur_buf + 1) % buf_count;
              if (lsp) {
                lsp_close(lsp);
                lsp_destroy(lsp);
                lsp = NULL;
              }
              if (fname_store[cur_buf][0]) {
                lsp = lsp_init(fname_store[cur_buf]);
                if (lsp)
                  lsp_open(lsp, &buf_store[cur_buf]);
                is_makefile = name_is_makefile(fname_store[cur_buf]);
              }
              update_git_info();
              cursor_row = 0;
              cursor_col = 0;
              scroll_row = 0;
            }
          } else if (strcmp(cmd_buf, ":bp") == 0) {
            if (buf_count > 1) {
              cur_buf = (cur_buf - 1 + buf_count) % buf_count;
              if (lsp) {
                lsp_close(lsp);
                lsp_destroy(lsp);
                lsp = NULL;
              }
              if (fname_store[cur_buf][0]) {
                lsp = lsp_init(fname_store[cur_buf]);
                if (lsp)
                  lsp_open(lsp, &buf_store[cur_buf]);
                is_makefile = name_is_makefile(fname_store[cur_buf]);
              }
              update_git_info();
              cursor_row = 0;
              cursor_col = 0;
              scroll_row = 0;
            }
          } else if (strcmp(cmd_buf, ":bd") == 0) {
            if (buf_count > 1) {
              buffer_destroy(&buf_store[cur_buf]);
              for (int i = cur_buf; i < buf_count - 1; i++) {
                buf_store[i] = buf_store[i + 1];
                strcpy(fname_store[i], fname_store[i + 1]);
              }
              buf_count--;
              if (cur_buf >= buf_count)
                cur_buf = buf_count - 1;
              if (lsp) {
                lsp_close(lsp);
                lsp_destroy(lsp);
                lsp = NULL;
              }
              if (fname_store[cur_buf][0]) {
                lsp = lsp_init(fname_store[cur_buf]);
                if (lsp)
                  lsp_open(lsp, &buf_store[cur_buf]);
                is_makefile = name_is_makefile(fname_store[cur_buf]);
              }
              update_git_info();
              cursor_row = 0;
              cursor_col = 0;
              scroll_row = 0;
            } else {
              snprintf(status_msg, sizeof(status_msg),
                       "Can't close last buffer");
              status_msg_time = SDL_GetTicks();
            }
          } else if (cmd_buf[0] == ':' && cmd_buf[1] == 'b' &&
                     cmd_buf[2] == ' ') {
            char *target = cmd_buf + 3;
            if (target[0] >= '0' && target[0] <= '9') {
              int n = atoi(target) - 1;
              if (n >= 0 && n < buf_count && n != cur_buf) {
                cur_buf = n;
                if (lsp) {
                  lsp_close(lsp);
                  lsp_destroy(lsp);
                  lsp = NULL;
                }
                if (fname_store[cur_buf][0]) {
                  lsp = lsp_init(fname_store[cur_buf]);
                  if (lsp)
                    lsp_open(lsp, &buf_store[cur_buf]);
                  is_makefile = name_is_makefile(fname_store[cur_buf]);
                }
                update_git_info();
                cursor_row = 0;
                cursor_col = 0;
                scroll_row = 0;
              }
            } else {
              int found = -1;
              for (int i = 0; i < buf_count; i++) {
                if (strstr(fname_store[i], target)) {
                  found = i;
                  break;
                }
              }
              if (found >= 0 && found != cur_buf) {
                cur_buf = found;
                if (lsp) {
                  lsp_close(lsp);
                  lsp_destroy(lsp);
                  lsp = NULL;
                }
                if (fname_store[cur_buf][0]) {
                  lsp = lsp_init(fname_store[cur_buf]);
                  if (lsp)
                    lsp_open(lsp, &buf_store[cur_buf]);
                  is_makefile = name_is_makefile(fname_store[cur_buf]);
                }
                update_git_info();
                cursor_row = 0;
                cursor_col = 0;
                scroll_row = 0;
              } else if (found < 0) {
                if (buf_count < 8) {
                  cur_buf = buf_count;
                  buf_store[cur_buf] = buffer_load(target);
                  strncpy(fname_store[cur_buf], target, 511);
                  buf_count++;
                  if (lsp) {
                    lsp_close(lsp);
                    lsp_destroy(lsp);
                    lsp = NULL;
                  }
                  lsp = lsp_init(target);
                  if (lsp)
                    lsp_open(lsp, &buf_store[cur_buf]);
                  is_makefile = name_is_makefile(target);
                  update_git_info();
                  cursor_row = 0;
                  cursor_col = 0;
                  scroll_row = 0;
                } else {
                  snprintf(status_msg, sizeof(status_msg), "Max 8 buffers");
                  status_msg_time = SDL_GetTicks();
                }
              }
            }
          } else if (strcmp(cmd_buf, ":ls") == 0) {
            char list[512] = "";
            for (int i = 0; i < buf_count; i++) {
              char ent[576];
              const char *nm = fname_store[i][0] ? fname_store[i] : "(new)";
              snprintf(ent, sizeof(ent), "%s%d:%.40s%s",
                       i == cur_buf ? "*" : "", i + 1, nm,
                       buf_store[i].dirty ? "+" : "");
              strncat(list, ent, sizeof(list) - strlen(list) - 1);
              if (i < buf_count - 1)
                strncat(list, " ", sizeof(list) - strlen(list) - 1);
            }
            snprintf(status_msg, sizeof(status_msg), "%s", list);
            status_msg_time = SDL_GetTicks();
          } else if (strcmp(cmd_buf, ":cc") == 0) {
            const char *src = fname_store[cur_buf];
            if (!src[0]) {
              snprintf(status_msg, sizeof(status_msg), "No file to compile");
              status_msg_time = SDL_GetTicks();
            } else if (buf_count >= 8) {
              snprintf(status_msg, sizeof(status_msg),
                       "Max 8 buffers, close one first");
              status_msg_time = SDL_GetTicks();
            } else {
              strncpy(make_saved_filename, src,
                      sizeof(make_saved_filename) - 1);
              char compiler_cmd[2048];
              char out_name[512];
              strncpy(out_name, src, sizeof(out_name) - 1);
              char *dot = strrchr(out_name, '.');
              if (dot)
                *dot = '\0';
              const char *ext = strrchr(src, '.');
              if (ext && (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 ||
                          strcmp(ext, ".cxx") == 0))
                snprintf(compiler_cmd, sizeof(compiler_cmd),
                         "g++ -Wall -Wextra -std=c++11 \"%s\" -o \"%s\" 2>&1",
                         src, out_name);
              else
                snprintf(compiler_cmd, sizeof(compiler_cmd),
                         "gcc -Wall -Wextra -std=c11 \"%s\" -o \"%s\" 2>&1",
                         src, out_name);
              FILE *cf = popen(compiler_cmd, "r");
              if (cf) {
                cur_buf = buf_count;
                buf_store[cur_buf] = buffer_create(100);
                buf_store[cur_buf].dirty = 0;
                fname_store[cur_buf][0] = '\0';
                char cline[4096];
                while (fgets(cline, sizeof(cline), cf)) {
                  cline[strcspn(cline, "\n")] = '\0';
                  if (buf_store[cur_buf].line_count >=
                      buf_store[cur_buf].capacity) {
                    buf_store[cur_buf].capacity *= 2;
                    buf_store[cur_buf].lines =
                        realloc(buf_store[cur_buf].lines,
                                sizeof(char *) * buf_store[cur_buf].capacity);
                  }
                  buf_store[cur_buf].lines[buf_store[cur_buf].line_count] =
                      strdup(cline);
                  buf_store[cur_buf].line_count++;
                }
                int exit_code = pclose(cf);
                buf_count++;
                showing_make = 1;
                exploring = 0;
                cursor_row = 0;
                cursor_col = 0;
                cursor_col_target = 0;
                scroll_row = 0;
                if (exit_code == 0) {
                  snprintf(status_msg, sizeof(status_msg),
                           "Compilation succeeded");
                  status_msg_time = SDL_GetTicks();
                }
              }
            }
          } else if (strcmp(cmd_buf, ":make") == 0 ||
                     strcmp(cmd_buf, ":m") == 0) {
            strncpy(make_saved_filename, fname_store[cur_buf],
                    sizeof(make_saved_filename) - 1);
            if (buf_count >= 8) {
              snprintf(status_msg, sizeof(status_msg),
                       "Max 8 buffers, close one first");
              status_msg_time = SDL_GetTicks();
            } else {
              FILE *mf = popen("make 2>&1", "r");
              if (mf) {
                cur_buf = buf_count;
                buf_store[cur_buf] = buffer_create(100);
                buf_store[cur_buf].dirty = 0;
                char mline[4096];
                while (fgets(mline, sizeof(mline), mf)) {
                  mline[strcspn(mline, "\n")] = '\0';
                  if (buf_store[cur_buf].line_count >=
                      buf_store[cur_buf].capacity) {
                    buf_store[cur_buf].capacity *= 2;
                    buf_store[cur_buf].lines =
                        realloc(buf_store[cur_buf].lines,
                                sizeof(char *) * buf_store[cur_buf].capacity);
                  }
                  buf_store[cur_buf].lines[buf_store[cur_buf].line_count] =
                      strdup(mline);
                  buf_store[cur_buf].line_count++;
                }
                pclose(mf);
                buf_count++;
              }
              showing_make = 1;
              exploring = 0;
              fname_store[cur_buf][0] = '\0';
              cursor_row = 0;
              cursor_col = 0;
              cursor_col_target = 0;
              scroll_row = 0;
            }
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

    /* ── Tab bar ─────────────────────────────── */
    SDL_SetRenderDrawColor(renderer, 31, 29, 46, 255);
    SDL_FRect tab_bg = {0, 0, (float)w, (float)TAB_BAR_HEIGHT};
    SDL_RenderFillRect(renderer, &tab_bg);
    int tab_x = CHAR_WIDTH / 2;
    for (int t = 0; t < buf_count; t++) {
      const char *disp = fname_store[t][0] ? fname_store[t] : "(new)";
      const char *base = strrchr(disp, '/');
      base = base ? base + 1 : disp;
      char label[576];
      snprintf(label, sizeof(label), " %s%s ", base,
               buf_store[t].dirty ? "*" : "");
      int tw = strlen(label) * CHAR_WIDTH;
      if (t == cur_buf) {
        SDL_SetRenderDrawColor(renderer, 64, 61, 82, 255);
        SDL_FRect active_tab = {tab_x - 2, 2, tw + 4, CHAR_HEIGHT + 4};
        SDL_RenderFillRect(renderer, &active_tab);
        SDL_SetTextureColorMod(fontTexture, 224, 222, 244);
      } else {
        SDL_SetTextureColorMod(fontTexture, 144, 140, 170);
      }
      for (int j = 0; label[j]; j++)
        DrawChar(renderer, fontTexture, label[j], tab_x + j * CHAR_WIDTH, 4);
      tab_x += tw + CHAR_WIDTH;
    }

    /* ── Gutter (line numbers) ──────────────────── */
    SDL_SetTextureColorMod(fontTexture, 144, 140, 170);
    SDL_SetRenderDrawColor(renderer, 31, 29, 46, 255);
    SDL_FRect gutter_rect = {0, (float)TAB_BAR_HEIGHT, (float)GUTTER_WIDTH,
                             (float)h - TAB_BAR_HEIGHT};
    SDL_RenderFillRect(renderer, &gutter_rect);

    /* ── Scroll clamp ───────────────────────────── */
    if (cursor_row < scroll_row) {
      scroll_row = cursor_row;
    }
    if (cursor_row >= scroll_row + max_visible_lines) {
      scroll_row = cursor_row - max_visible_lines + 1;
    }

    /* ── Text lines ─────────────────────────────── */
    for (int i = scroll_row; i < buf_store[cur_buf].line_count; i++) {
      int y = (i - scroll_row) * CHAR_HEIGHT + TAB_BAR_HEIGHT;
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

      // cursor line highlight
      if (i == cursor_row) {
        SDL_SetRenderDrawColor(renderer, 64, 61, 82, 255);
        SDL_FRect line_bg = {(float)GUTTER_WIDTH, (float)y,
                             (float)(w - GUTTER_WIDTH), (float)CHAR_HEIGHT};
        SDL_RenderFillRect(renderer, &line_bg);
      }

      // draw text content
      int line_len = strlen(buf_store[cur_buf].lines[i]);

      // search highlighting
      if (last_search[0]) {
        char *p = buf_store[cur_buf].lines[i];
        while ((p = strstr(p, last_search)) != NULL) {
          int match_col = p - buf_store[cur_buf].lines[i];
          int match_len = strlen(last_search);
          for (int sj = 0; sj < match_len; sj++) {
            SDL_SetRenderDrawColor(renderer, 82, 79, 103, 255);
            SDL_FRect m_bg = {GUTTER_WIDTH + (match_col + sj) * CHAR_WIDTH,
                              (float)y, (float)CHAR_WIDTH, (float)CHAR_HEIGHT};
            SDL_RenderFillRect(renderer, &m_bg);
          }
          p += match_len;
        }
      }

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
        int color = classify_char(buf_store[cur_buf].lines[i], j);
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
        DrawChar(renderer, fontTexture, buf_store[cur_buf].lines[i][j],
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
                snprintf(status_msg, sizeof(status_msg), "Go to definition");
                status_msg_time = SDL_GetTicks();
              }
            } else {
              snprintf(status_msg, sizeof(status_msg), "No definition found");
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
            filter_completions(buf_store[cur_buf].lines[cursor_row],
                               cursor_col);
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
    cursor.y = (cursor_row - scroll_row) * CHAR_HEIGHT + TAB_BAR_HEIGHT;

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
      mode_r = 196;
      mode_g = 167;
      mode_b = 231;
      break;
    case INSERT:
      mode_str = "INSERT";
      mode_r = 156;
      mode_g = 207;
      mode_b = 216;
      break;
    case VISUAL:
      mode_str = "VISUAL";
      mode_r = 235;
      mode_g = 111;
      mode_b = 146;
      break;
    case COMMAND:
      mode_str = "COMMAND";
      mode_r = 246;
      mode_g = 193;
      mode_b = 119;
      break;
    }

    int sx = CHAR_WIDTH;
    int offset = 0;

    if (status_msg[0] && SDL_GetTicks() - status_msg_time < 2000) {
      int is_err = strstr(status_msg, "not found") != NULL;
      Uint8 r = is_err ? 235 : 196, g = is_err ? 111 : 167,
            b = is_err ? 146 : 231;
      SDL_SetTextureColorMod(fontTexture, r, g, b);
      for (int j = 0; status_msg[j]; j++) {
        DrawChar(renderer, fontTexture, status_msg[j], sx + offset * CHAR_WIDTH,
                 status_y);
        offset++;
      }
    } else {
      status_msg[0] = '\0';
    }

    if (offset == 0) {
      if (save_feedback_time && SDL_GetTicks() - save_feedback_time < 1500) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "saved!  |  %s",
                 fname_store[cur_buf][0] ? fname_store[cur_buf] : "(new)");
        SDL_SetTextureColorMod(fontTexture, 156, 207, 216);
        for (int j = 0; tmp[j]; j++) {
          DrawChar(renderer, fontTexture, tmp[j], sx + offset * CHAR_WIDTH,
                   status_y);
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
          DrawChar(renderer, fontTexture, cmd_buf[j], sx + offset * CHAR_WIDTH,
                   status_y);
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
          DrawChar(renderer, fontTexture, sep[j], sx + offset * CHAR_WIDTH,
                   status_y);
          offset++;
        }
        SDL_SetTextureColorMod(fontTexture, mode_r, mode_g, mode_b);
        for (int j = 0; mode_str[j]; j++) {
          DrawChar(renderer, fontTexture, mode_str[j], sx + offset * CHAR_WIDTH,
                   status_y);
          offset++;
        }
      } else {
        const char *fn =
            fname_store[cur_buf][0] ? fname_store[cur_buf] : "(new)";
        SDL_SetTextureColorMod(fontTexture, 156, 207, 216);
        for (int j = 0; fn[j]; j++) {
          DrawChar(renderer, fontTexture, fn[j], sx + offset * CHAR_WIDTH,
                   status_y);
          offset++;
        }
        if (buf_store[cur_buf].dirty) {
          const char *dirty_tag = "[+]";
          SDL_SetTextureColorMod(fontTexture, 246, 193, 119);
          for (int j = 0; dirty_tag[j]; j++) {
            DrawChar(renderer, fontTexture, dirty_tag[j],
                     sx + offset * CHAR_WIDTH, status_y);
            offset++;
          }
          SDL_SetTextureColorMod(fontTexture, 156, 207, 216);
        }
        if (git_branch[0]) {
          char branch_str[80];
          snprintf(branch_str, sizeof(branch_str), "  (%s)", git_branch);
          SDL_SetTextureColorMod(fontTexture, 196, 167, 231);
          for (int j = 0; branch_str[j]; j++) {
            DrawChar(renderer, fontTexture, branch_str[j],
                     sx + offset * CHAR_WIDTH, status_y);
            offset++;
          }
          SDL_SetTextureColorMod(fontTexture, 156, 207, 216);
        }
        char pos[64];
        snprintf(pos, sizeof(pos), "  |  Line %d, Col %d  |  ", cursor_row + 1,
                 cursor_col + 1);
        SDL_SetTextureColorMod(fontTexture, 144, 140, 170);
        for (int j = 0; pos[j]; j++) {
          DrawChar(renderer, fontTexture, pos[j], sx + offset * CHAR_WIDTH,
                   status_y);
          offset++;
        }
        SDL_SetTextureColorMod(fontTexture, mode_r, mode_g, mode_b);
        for (int j = 0; mode_str[j]; j++) {
          DrawChar(renderer, fontTexture, mode_str[j], sx + offset * CHAR_WIDTH,
                   status_y);
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
              DrawChar(renderer, fontTexture, diag[j], sx + offset * CHAR_WIDTH,
                       status_y);
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
        if (idx >= completion_count)
          break;
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
  buffer_destroy(&buf_store[cur_buf]);
  TTF_CloseFont(font);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}
