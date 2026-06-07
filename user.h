#pragma once

typedef enum {
  NORMAL,
  INSERT,
  VISUAL,
  COMMAND,
} State;

typedef struct {
  State state;
} User;

User create_user(void);
