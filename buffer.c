#include "buffer.h"
#include <SDL3/SDL_stdinc.h>

Buffer buffer_create(int initial_capacity) {
  Buffer buf;
  buf.capacity = initial_capacity;
  buf.line_count = 1;
  buf.dirty = 0;
  buf.lines = malloc(sizeof(char *) * initial_capacity);
  buf.lines[0] = malloc(1);
  buf.lines[0][0] = '\0';
  return buf;
}

void buffer_destroy(Buffer *buf) {
  for (int i = 0; i < buf->line_count; i++) {
    free(buf->lines[i]);
  }
  free(buf->lines);
}

void buffer_insert_char(Buffer *buf, int row, int col, char c) {
  char *line = buf->lines[row];
  int len = strlen(line);

  buf->lines[row] = realloc(line, len + 2);
  line = buf->lines[row];

  if (col > len) { // ← right here
    col = len;     // clamp col to end of line
  }

  memmove(line + col + 1, line + col, len - col + 1);
  line[col] = c;
  buf->dirty = 1;
}

void buffer_delete_char(Buffer *buf, int row, int col) {
  char *line = buf->lines[row];
  int len = strlen(line);
  if (col <= 0)
    return;

  if (col > len)
    return;

  memmove(line + col - 1, line + col, len - col + 1);
  buf->lines[row] = realloc(line, len);
  buf->dirty = 1;
}

void buffer_insert_line(Buffer *buf, int row) {
  if (buf->line_count >= buf->capacity) {
    buf->capacity *= 2;
    buf->lines = realloc(buf->lines, sizeof(char *) * buf->capacity);
  }

  memmove(&buf->lines[row + 1], &buf->lines[row],
          sizeof(char *) * (buf->line_count - row));

  buf->lines[row] = malloc(1);
  buf->lines[row][0] = '\0';

  buf->line_count++;
  buf->dirty = 1;
}

Buffer buffer_load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return buffer_create(100);

  char line[4096];
  Buffer buf = buffer_create(100);
  buf.dirty = 0;
  if (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    free(buf.lines[0]);
    buf.lines[0] = malloc(strlen(line) + 1);
    strcpy(buf.lines[0], line);
  }

  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';

    if (buf.line_count >= buf.capacity) {
      buf.capacity *= 2;
      buf.lines = realloc(buf.lines, sizeof(char *) * buf.capacity);
    }
    buf.lines[buf.line_count] = malloc(strlen(line) + 1);
    strcpy(buf.lines[buf.line_count], line);
    buf.line_count++;
  }
  fclose(f);
  return buf;
}

void buffer_save(Buffer *buf, const char *path) {
  FILE *f = fopen(path, "w");

  if (!f) {
    return;
  }
  for (int i = 0; i < buf->line_count; i++) {
    fprintf(f, "%s\n", buf->lines[i]);
  }
  fclose(f);
  buf->dirty = 0;
}

void buffer_delete_line(Buffer *buf, int row) {
  if (buf->line_count <= 1) {
    return;
  }
  free(buf->lines[row]);

  memmove(&buf->lines[row], &buf->lines[row + 1],
          sizeof(char *) * (buf->line_count - row - 1));

  buf->line_count--;
  buf->dirty = 1;
}

Buffer buffer_clone(Buffer *src) {
  Buffer c;
  c.line_count = src->line_count;
  c.capacity = src->capacity;
  c.lines = malloc(sizeof(char *) * c.capacity);
  for (int i = 0; i < c.line_count; i++)
    c.lines[i] = strdup(src->lines[i]);
  c.dirty = src->dirty;
  return c;
}

void buffer_destroy_clone(Buffer *buf) {
  for (int i = 0; i < buf->line_count; i++)
    free(buf->lines[i]);
  free(buf->lines);
}
