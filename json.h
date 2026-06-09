#ifndef JSON_H
#define JSON_H

typedef enum {
  JSON_STR,
  JSON_NUM,
  JSON_OBJ,
  JSON_ARR,
  JSON_BOOL,
  JSON_NULL
} JsonType;

typedef struct JsonValue JsonValue;
struct JsonValue {
  JsonType type;
  union {
    char *string;
    double number;
    struct {
      char **keys;
      JsonValue **values;
      int count;
    } obj;
    struct {
      JsonValue **items;
      int count;
    } arr;
    int boolean;
  };
};

JsonValue *json_parse(const char *text);
void json_free(JsonValue *v);
JsonValue *json_get(JsonValue *obj, const char *key);
char *json_get_string(JsonValue *obj, const char *key);

#endif
