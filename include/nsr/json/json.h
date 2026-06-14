#ifndef NSR_JSON_H
#define NSR_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} nsr_json_buf_t;

void nsr_json_init(nsr_json_buf_t *jb);
void nsr_json_reset(nsr_json_buf_t *jb);
void nsr_json_free(nsr_json_buf_t *jb);
const char *nsr_json_cstr(const nsr_json_buf_t *jb);

void nsr_json_obj_start(nsr_json_buf_t *jb);
void nsr_json_obj_end(nsr_json_buf_t *jb);
void nsr_json_arr_start(nsr_json_buf_t *jb);
void nsr_json_arr_end(nsr_json_buf_t *jb);

void nsr_json_key(nsr_json_buf_t *jb, const char *key);
void nsr_json_string(nsr_json_buf_t *jb, const char *val);
void nsr_json_int(nsr_json_buf_t *jb, long long val);
void nsr_json_double(nsr_json_buf_t *jb, double val);
void nsr_json_bool(nsr_json_buf_t *jb, bool val);
void nsr_json_null(nsr_json_buf_t *jb);
void nsr_json_append_raw(nsr_json_buf_t *jb, const char *s, size_t n);

/* Decode helpers: value pointers are into the original JSON string. */
const char *nsr_json_obj_get(const char *json, const char *key, size_t *out_len);
bool nsr_json_parse_str(const char *val, size_t val_len, char *out, size_t out_len);
bool nsr_json_parse_int(const char *val, size_t val_len, long long *out);
bool nsr_json_parse_double(const char *val, size_t val_len, double *out);
bool nsr_json_parse_bool(const char *val, size_t val_len, bool *out);

/* Array iteration. Returns pointer to element and updates next pointer. */
const char *nsr_json_arr_first(const char *json, const char **next);
const char *nsr_json_arr_next(const char *next, const char **new_next);

#endif
