#ifndef BUFFER_H
#define BUFFER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char **lines;
  int line_count;
  int capacity;
  int dirty;
} Buffer;

Buffer buffer_create(int initial_capacity);
void buffer_insert_char(Buffer *buf, int row, int col, char c);
void buffer_delete_char(Buffer *buf, int row, int col);
void buffer_insert_line(Buffer *buf, int row);
void buffer_delete_line(Buffer *buf, int row);
void buffer_destroy(Buffer *buf);
Buffer buffer_load(const char *path);
void buffer_save(Buffer *buf, const char *path);
Buffer buffer_clone(Buffer *src);
void buffer_destroy_clone(Buffer *buf);

#endif
