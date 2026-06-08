#include "buffer.h"
#include "user.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
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

int CHAR_WIDTH = 8;
int CHAR_HEIGHT = 16;

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
#define UNDO_MAX 100
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

int main(int argc, char *argv[]) {
  Cursor cursor;
  Buffer buf;

  char *filename = NULL;
  if (argc > 1) {
    filename = argv[1];
    buf = buffer_load(filename);
  } else {
    buf = buffer_create(300);
  }

  User user = create_user();

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
  bool swallow_text = false;
  // Vim Keymaps needed
  char pending_operator = 0;
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
          if (swallow_text) {
            swallow_text = false;
          } else if (event.text.text[0] >= 32) {
            buffer_insert_char(&buf, cursor_row, cursor_col,
                               event.text.text[0]);
            cursor_col++;
          }
        } else if (user.state == NORMAL && event.text.text[0] == ':') {
          user.state = COMMAND;
          cmd_len = 1;
          cmd_buf[0] = ':';
          cmd_buf[1] = '\0';
        } else if (user.state == COMMAND && cmd_len < 255 &&
                   event.text.text[0] >= 32) {
          cmd_buf[cmd_len++] = event.text.text[0];
          cmd_buf[cmd_len] = '\0';
        }
      }
      if (event.type == SDL_EVENT_KEY_DOWN) {
        cursor_visible = true;
        blink_timer = SDL_GetTicks();
        if (user.state != NORMAL && event.key.key == SDLK_ESCAPE) {
          user.state = NORMAL;
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
          cursor_row -= max_visible_lines;
          if (cursor_row < 0)
            cursor_row = 0;
        }
        if (event.key.key == SDLK_PAGEDOWN) {
          cursor_row += max_visible_lines;
          if (cursor_row >= buf.line_count)
            cursor_row = buf.line_count - 1;
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
          if (event.key.key == SDLK_K) {
            if (cursor_row != 0) {
              cursor_row--;
            }
            int line_len = strlen(buf.lines[cursor_row]);
            cursor_col = min(cursor_col_target, line_len);
          }
          if (event.key.key == SDLK_J) {
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
          if (event.key.key >= SDLK_1 && event.key.key <= SDLK_9) {
            prefix_count = prefix_count * 10 + (event.key.key - SDLK_1 + 1);
          }
          if (event.key.key == SDLK_0 && prefix_count == 0) {
            cursor_col = 0;
            cursor_col_target = 0;
          } else if (event.key.key == SDLK_0) {
            prefix_count *= 10;
          }

          // SWITCHING MODE TO INSERT
          if (event.key.key == SDLK_I) {
            push_undo(&buf, cursor_row, cursor_col);
            user.state = INSERT;
            swallow_text = true;
          }
          // SWITCHING MODE TO VISUAL
          if (event.key.key == SDLK_V) {
            visual_anchor_row = cursor_row;
            visual_anchor_col = cursor_col;
            user.state = VISUAL;
          }
          if (event.key.key == SDLK_X) {
            push_undo(&buf, cursor_row, cursor_col);
            int count = prefix_count > 0 ? prefix_count : 1;
            for (int c = 0; c < count; c++) {
              int line_len = strlen(buf.lines[cursor_row]);
              if (cursor_col >= line_len) {
                break;
              }
              buffer_delete_char(&buf, cursor_row, cursor_col);
            }
            prefix_count = 0;
          }
        }
        if (event.key.key == SDLK_G && (event.key.mod & SDL_KMOD_SHIFT)) {
          cursor_row = buf.line_count - 1;
        }
        if (event.key.key == SDLK_D) {
          if (pending_operator == 'd') {
            push_undo(&buf, cursor_row, cursor_col);
            int count = prefix_count > 0 ? prefix_count : 1;
            for (int c = 0; c < count; c++) {
              if (buf.line_count <= 1) {
                break;
              }
              buffer_delete_line(&buf, cursor_row);
              if (cursor_row >= buf.line_count) {
                cursor_row = buf.line_count - 1;
              }
              cursor_col = 0;
              pending_operator = 0;
              prefix_count = 0;
            }
            buffer_delete_line(&buf, cursor_row);
            if (cursor_row > 0) {
              cursor_row -= 1;
            }
            cursor_col = 0;
            pending_operator = 0;
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
            }
          }
          if (event.key.key == SDLK_O && (event.key.mod & SDL_KMOD_SHIFT)) {
            // O — open line ABOVE
            push_undo(&buf, cursor_row, cursor_col);
            buffer_insert_line(&buf, cursor_row);
            user.state = INSERT;
            cursor_col = 0;
            swallow_text = true;
          } else if (event.key.key == SDLK_O) {
            push_undo(&buf, cursor_row, cursor_col);
            buffer_insert_line(&buf, cursor_row + 1);
            user.state = INSERT;
            cursor_row++;
            cursor_col = 0;
            swallow_text = true;
          }
          if (event.key.key == SDLK_A) {
            push_undo(&buf, cursor_row, cursor_col);
            cursor_col = strlen(buf.lines[cursor_row]);
            user.state = INSERT;
            swallow_text = true;
          }
        }
      }
      if (user.state == INSERT) {
        if (event.key.key == SDLK_TAB && SDL_GetTicks() - last_tab_time > 100) {
          last_tab_time = SDL_GetTicks();
          buffer_insert_char(&buf, cursor_row, cursor_col, ' ');
          cursor_col++;
          buffer_insert_char(&buf, cursor_row, cursor_col, ' ');
          cursor_col++;
          swallow_text = true;
        }
        if (event.key.key == SDLK_BACKSPACE &&
            SDL_GetTicks() - last_bksp_time > 100) {
          last_bksp_time = SDL_GetTicks();
          if (cursor_col > 0) {
            buffer_delete_char(&buf, cursor_row, cursor_col);
            cursor_col--;
          } else if (cursor_row > 0) {
            // merge with previous line
            int prev_len = strlen(buf.lines[cursor_row - 1]);
            int cur_len = strlen(buf.lines[cursor_row]);
            buf.lines[cursor_row - 1] =
                realloc(buf.lines[cursor_row - 1], prev_len + cur_len + 1);
            memcpy(buf.lines[cursor_row - 1] + prev_len, buf.lines[cursor_row],
                   cur_len + 1);
            buffer_delete_line(&buf, cursor_row);
            cursor_row--;
            cursor_col = prev_len;
          }
        }
        if (event.key.key == SDLK_RETURN &&
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
          swallow_text = true;
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
        if (event.key.key == SDLK_RETURN) {
          if (strcmp(cmd_buf, ":w") == 0)
            buffer_save(&buf, filename);
          else if (strcmp(cmd_buf, ":q") == 0)
            running = false;
          else if (strcmp(cmd_buf, ":wq") == 0) {
            buffer_save(&buf, filename);
            running = false;
          }
          user.state = NORMAL;
        }
        if (event.key.key == SDLK_BACKSPACE && cmd_len > 0) {
          cmd_buf[--cmd_len] = '\0';
        }
      }
    }

    /* ── Background ─────────────────────────────── */
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderClear(renderer);

    SDL_SetTextureColorMod(fontTexture, 255, 255, 255);
    /* ── Gutter (line numbers) ──────────────────── */
    const int GUTTER_WIDTH = 4 * CHAR_WIDTH;
    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
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
            SDL_SetRenderDrawColor(renderer, 60, 60, 120, 255);
            SDL_FRect bg = {GUTTER_WIDTH + j * CHAR_WIDTH, y, CHAR_WIDTH,
                            CHAR_HEIGHT};
            SDL_RenderFillRect(renderer, &bg);
          }
        }
        int color = classify_char(buf.lines[i], j);
        switch (color) {
        case 1:
          SDL_SetTextureColorMod(fontTexture, 230, 180, 80);
          break;
        case 2:
          SDL_SetTextureColorMod(fontTexture, 100, 130, 100);
          break;
        case 3:
          SDL_SetTextureColorMod(fontTexture, 150, 200, 150);
          break;
        case 4:
          SDL_SetTextureColorMod(fontTexture, 200, 160, 100);
          break;
        default:
          SDL_SetTextureColorMod(fontTexture, 255, 255, 255);
          break;
        }
        DrawChar(renderer, fontTexture, buf.lines[i][j],
                 GUTTER_WIDTH + j * CHAR_WIDTH, y);
        SDL_SetTextureColorMod(fontTexture, 255, 255, 255);
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
        SDL_SetRenderDrawColor(renderer, 100, 255, 100, 255);
        SDL_FRect cursor_rect = {cursor.x, cursor.y, 2.0f, (float)CHAR_HEIGHT};
        SDL_RenderFillRect(renderer, &cursor_rect);
      } else {
        // full block (NORMAL, COMMAND, VISUAL)
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_FRect cursor_rect = {cursor.x, cursor.y, (float)CHAR_WIDTH,
                                 (float)CHAR_HEIGHT};
        SDL_RenderFillRect(renderer, &cursor_rect);
      }
    }

    /* ── Status bar ─────────────────────────────── */
    float status_y = h - CHAR_HEIGHT;
    SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
    SDL_FRect status_rect = {0, status_y, (float)w, (float)CHAR_HEIGHT};
    SDL_RenderFillRect(renderer, &status_rect);

    const char *mode_str = "";
    switch (user.state) {
    case NORMAL:
      mode_str = "NORMAL";
      break;
    case INSERT:
      mode_str = "INSERT";
      break;
    case VISUAL:
      mode_str = "VISUAL";
      break;
    case COMMAND:
      mode_str = "COMMAND";
      break;
    }

    char status[512];
    if (user.state == COMMAND) {
      snprintf(status, sizeof(status), "%s", cmd_buf);
    } else {
      snprintf(status, sizeof(status), "Cim  |  %s  |  Line %d, Col %d  |  %s",
               filename ? filename : "(new)", cursor_row + 1, cursor_col + 1,
               mode_str);
    }

    int sx = CHAR_WIDTH;
    for (int j = 0; status[j]; j++) {
      DrawChar(renderer, fontTexture, status[j], sx + j * CHAR_WIDTH, status_y);
    }

    /* ── Present ────────────────────────────────── */
    SDL_RenderPresent(renderer);
  }
  buffer_destroy(&buf);
  TTF_CloseFont(font);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}
