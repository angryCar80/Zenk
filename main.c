#include "buffer.h"
#include "user.h"
#include <SDL3/SDL.h>
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
#include <string.h>

int CHAR_WIDTH = 8;
int CHAR_HEIGHT = 16;

typedef struct {
  float x;
  float y;
} Cursor;

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
          } else {
            buffer_insert_char(&buf, cursor_row, cursor_col,
                               event.text.text[0]);
            cursor_col++;
          }
        } else if (user.state == NORMAL && event.text.text[0] == ':') {
          user.state = COMMAND;
          cmd_len = 1;
          cmd_buf[0] = ':';
          cmd_buf[1] = '\0';
        } else if (user.state == COMMAND && cmd_len < 255) {
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

        if (user.state == NORMAL) {
          if (pending_operator != 0 && event.key.key != SDLK_D &&
              event.key.key != SDLK_Y) {
            pending_operator = 0;
          }
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
            if (cursor_row != buf.line_count - 1) {
              cursor_row++;
            }
            int line_len = strlen(buf.lines[cursor_row]);
            cursor_col = min(cursor_col_target, line_len);
          }
          if (event.key.key == SDLK_I) {
            user.state = INSERT;
            swallow_text = true;
          }
          if (event.key.key == SDLK_X) {
            buffer_delete_char(&buf, cursor_row, cursor_col);
          }
          if (event.key.key == SDLK_D) {
            if (pending_operator == 'd') {
              buffer_delete_line(&buf, cursor_row);
              if (cursor_row > 0) {
                cursor_row -= 1;
              }
              cursor_col = 0;
              pending_operator = 0;
            } else {
              pending_operator = 'd';
            }
          } else {
            pending_operator = 0;
          }
          if (event.key.key == SDLK_O) {
            buffer_insert_line(&buf, cursor_row);
            user.state = INSERT;
            cursor_row++;
            swallow_text = true;
          }
        }
        if (user.state == INSERT) {
          if (event.key.key == SDLK_BACKSPACE) {
            if (cursor_col > 0) {
              buffer_delete_char(&buf, cursor_row, cursor_col);
              cursor_col--;
            } else if (cursor_row > 0) {
              // merge with previous line
              int prev_len = strlen(buf.lines[cursor_row - 1]);
              int cur_len = strlen(buf.lines[cursor_row]);
              buf.lines[cursor_row - 1] =
                  realloc(buf.lines[cursor_row - 1], prev_len + cur_len + 1);
              memcpy(buf.lines[cursor_row - 1] + prev_len,
                     buf.lines[cursor_row], cur_len + 1);
              buffer_delete_line(&buf, cursor_row);
              cursor_row--;
              cursor_col = prev_len;
            }
          }
          if (event.key.key == SDLK_RETURN) {
            int len = strlen(buf.lines[cursor_row]);

            int tail_len = len - cursor_col;

            buffer_insert_line(&buf, cursor_row + 1);

            char *new_line = malloc(tail_len + 1);
            memcpy(new_line, buf.lines[cursor_row] + cursor_col, tail_len);
            new_line[tail_len] = '\0';
            buf.lines[cursor_row + 1] = new_line;

            buf.lines[cursor_row] =
                realloc(buf.lines[cursor_row], cursor_col + 1);
            buf.lines[cursor_row][cursor_col] = '\0';

            cursor_row++;
            cursor_col = 0;
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
    }

    /* ── Background ─────────────────────────────── */
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderClear(renderer);

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
      snprintf(num_str, sizeof(num_str), "%d", i + 1);
      int num_len = strlen(num_str);
      int num_x = GUTTER_WIDTH - (num_len * CHAR_WIDTH) - CHAR_WIDTH;
      for (int j = 0; j < num_len; j++) {
        DrawChar(renderer, fontTexture, num_str[j], num_x + j * CHAR_WIDTH, y);
      }

      // draw text content
      int line_len = strlen(buf.lines[i]);
      for (int j = 0; j < line_len; j++) {
        DrawChar(renderer, fontTexture, buf.lines[i][j],
                 GUTTER_WIDTH + j * CHAR_WIDTH, y);
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
