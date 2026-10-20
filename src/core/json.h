#ifndef ODIN_JSON_H
#define ODIN_JSON_H

#include <stdbool.h>
#include <stddef.h>

// small json reader that builds a tree. enough for api responses, not a speed demon

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct json {
    jtype t;
    bool b;
    double n;
    char *s;             // J_STR
    struct json **items; // J_ARR and J_OBJ values
    char **keys;         // J_OBJ keys, same order as items
    size_t len;
} json;

json *json_parse(const char *s, size_t n); // NULL if it's broken
void json_free(json *j);
json *json_get(const json *obj, const char *key);    // NULL if missing / not an object
const char *json_str(const json *obj, const char *key); // NULL unless it's a string
bool json_num(const json *obj, const char *key, double *out); // false unless it's a number

// write s as a quoted json string to stdout (used by the -j output everywhere)
void json_quote(const char *s);

#endif
