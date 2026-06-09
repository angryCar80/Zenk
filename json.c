#include "json.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) {
  while (*p && isspace((unsigned char)*p)) {
    p++;
  }
  return p;
}

static char *parse_string(const char **pp) {
  const char *p = *pp;
  if (*p != '"') {
    return NULL;
  }
  p++;
  int len = 0;
  while (p[len] && p[len] != '"') {
    if (p[len] == '\\') {
      len++;
    }
    len++;
  }
  char *s = malloc(len + 1);
  int j = 0;
  while (*p && *p != '"') {
    if (*p == '\\') {
      p++;
      switch (*p) {
      case '"':
        s[j++] = '"';
        break;
      case '\\':
        s[j++] = '\\';
        break;
      case 'n':
        s[j++] = '\n';
        break;
      case 't':
        s[j++] = '\t';
        break;
      case 'r':
        s[j++] = '\r';
        break;
      default:
        s[j++] = *p;
        break;
      }
    } else {
      s[j++] = *p;
    }
    p++;
  }
  s[j] = '\0';
  if (*p == '"') {
    p++;
  }
  *pp = p;
  return s;
}

static JsonValue *parse_value(const char **pp);

static JsonValue *parse_value(const char **pp) {
  const char *p = skip_ws(*pp);
  if (!*p)
    return NULL;

  JsonValue *v = calloc(1, sizeof(JsonValue));

  if (*p == '"') {
    v->type = JSON_STR;
    v->string = parse_string(&p);
  } else if (*p == '{') {
    v->type = JSON_OBJ;
    p++;
    // count keys first (simple pass)
    // ... or grow dynamically
    int cap = 8;
    v->obj.keys = malloc(sizeof(char *) * cap);
    v->obj.values = malloc(sizeof(JsonValue *) * cap);
    v->obj.count = 0;
    p = skip_ws(p);
    if (*p != '}') {
      while (1) {
        p = skip_ws(p);
        if (v->obj.count >= cap) {
          cap *= 2;
          v->obj.keys = realloc(v->obj.keys, sizeof(char *) * cap);
          v->obj.values = realloc(v->obj.values, sizeof(JsonValue *) * cap);
        }
        v->obj.keys[v->obj.count] = parse_string(&p);
        p = skip_ws(p);
        if (*p == ':')
          p++;
        v->obj.values[v->obj.count] = parse_value(&p);
        v->obj.count++;
        p = skip_ws(p);
        if (*p == ',') {
          p++;
          continue;
        }
        if (*p == '}')
          break;
      }
    }
    if (*p == '}')
      p++;
  } else if (*p == '[') {
    v->type = JSON_ARR;
    p++;
    int cap = 8;
    v->arr.items = malloc(sizeof(JsonValue *) * cap);
    v->arr.count = 0;
    p = skip_ws(p);
    if (*p != ']') {
      while (1) {
        if (v->arr.count >= cap) {
          cap *= 2;
          v->arr.items = realloc(v->arr.items, sizeof(JsonValue *) * cap);
        }
        v->arr.items[v->arr.count++] = parse_value(&p);
        p = skip_ws(p);
        if (*p == ',') {
          p++;
          continue;
        }
        if (*p == ']')
          break;
      }
    }
    if (*p == ']')
      p++;
  } else if (*p == 't' && strncmp(p, "true", 4) == 0) {
    v->type = JSON_BOOL;
    v->boolean = 1;
    p += 4;
  } else if (*p == 'f' && strncmp(p, "false", 5) == 0) {
    v->type = JSON_BOOL;
    v->boolean = 0;
    p += 5;
  } else if (*p == 'n' && strncmp(p, "null", 4) == 0) {
    v->type = JSON_NULL;
    p += 4;
  } else if (*p == '-' || isdigit((unsigned char)*p)) {
    v->type = JSON_NUM;
    v->number = strtod(p, (char **)&p);
  } else {
    free(v);
    return NULL;
  }
  *pp = p;
  return v;
}
JsonValue *json_parse(const char *text) {
  const char *p = text;
  return parse_value(&p);
}
void json_free(JsonValue *v) {
  if (!v)
    return;
  if (v->type == JSON_STR)
    free(v->string);
  else if (v->type == JSON_OBJ) {
    for (int i = 0; i < v->obj.count; i++) {
      free(v->obj.keys[i]);
      json_free(v->obj.values[i]);
    }
    free(v->obj.keys);
    free(v->obj.values);
  } else if (v->type == JSON_ARR) {
    for (int i = 0; i < v->arr.count; i++) {
      json_free(v->arr.items[i]);
    }
    free(v->arr.items);
  }
  free(v);
}

JsonValue *json_get(JsonValue *obj, const char *key) {
  if (!obj || obj->type != JSON_OBJ)
    return NULL;
  for (int i = 0; i < obj->obj.count; i++) {
    if (strcmp(obj->obj.keys[i], key) == 0)
      return obj->obj.values[i];
  }
  return NULL;
}

char *json_get_string(JsonValue *obj, const char *key) {
  JsonValue *v = json_get(obj, key);
  if (!v || v->type != JSON_STR)
    return NULL;
  return v->string;
}
