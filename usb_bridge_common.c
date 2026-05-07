#define _GNU_SOURCE
#include "usb_bridge_common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static char now_buf[32];

void sb_init(strbuf_t *b) { b->data = NULL; b->len = 0; }

void sb_free(strbuf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
}

void sb_appendn(strbuf_t *b, const char *s, size_t n) {
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return;
    b->data = p;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void sb_append(strbuf_t *b, const char *s) {
    if (s) sb_appendn(b, s, strlen(s));
}

void sb_printf(strbuf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *tmp = NULL;
    int n = vasprintf(&tmp, fmt, ap);
    va_end(ap);
    if (n >= 0 && tmp) {
        sb_appendn(b, tmp, (size_t)n);
        free(tmp);
    }
}

const char *now_text(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(now_buf, sizeof(now_buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return now_buf;
}

long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

int path_exists(const char *path) {
    return access(path, F_OK) == 0;
}

void ensure_dir(const char *path) {
    if (!path || !*path) return;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void ensure_parent_dir(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *p = strrchr(tmp, '/');
    if (p) {
        *p = 0;
        ensure_dir(tmp);
    }
}

void log_line(const char *log_file, const char *fmt, ...) {
    ensure_parent_dir(log_file);
    va_list ap;
    va_start(ap, fmt);
    char *msg = NULL;
    int n = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (n < 0 || !msg) return;

    fprintf(stdout, "[%s] %s\n", now_text(), msg);
    fflush(stdout);

    FILE *f = fopen(log_file, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", now_text(), msg);
        fclose(f);
    }
    free(msg);
}

void ui_status_update(const char *phase, const char *title, const char *message,
                      const char *scan_text, const char *patient_id, const char *report_no) {
    strbuf_t b;
    sb_init(&b);
    sb_append(&b, "{\n  \"time\": ");
    json_escape_append(&b, now_text());
    sb_append(&b, ",\n  \"phase\": ");
    json_escape_append(&b, phase ? phase : "idle");
    sb_append(&b, ",\n  \"title\": ");
    json_escape_append(&b, title ? title : "");
    sb_append(&b, ",\n  \"message\": ");
    json_escape_append(&b, message ? message : "");
    sb_append(&b, ",\n  \"scan_text\": ");
    json_escape_append(&b, scan_text ? scan_text : "");
    sb_append(&b, ",\n  \"patient_id\": ");
    json_escape_append(&b, patient_id ? patient_id : "");
    sb_append(&b, ",\n  \"report_no\": ");
    json_escape_append(&b, report_no ? report_no : "");
    sb_append(&b, "\n}\n");
    atomic_write_file(UI_STATUS_FILE, b.data ? b.data : "", b.len);
    sb_free(&b);
}

void ui_status_update_projects(const char *scan_text, const char *patient_id, const char *report_no,
                               int selected_index, int project_count, const char **projects) {
    strbuf_t b;
    sb_init(&b);
    sb_append(&b, "{\n  \"time\": ");
    json_escape_append(&b, now_text());
    sb_append(&b, ",\n  \"phase\": \"select_project\"");
    sb_append(&b, ",\n  \"title\": \"Select exam item\"");
    sb_append(&b, ",\n  \"message\": \"Use previous/next/confirm keys to continue\"");
    sb_append(&b, ",\n  \"scan_text\": ");
    json_escape_append(&b, scan_text ? scan_text : "");
    sb_append(&b, ",\n  \"patient_id\": ");
    json_escape_append(&b, patient_id ? patient_id : "");
    sb_append(&b, ",\n  \"report_no\": ");
    json_escape_append(&b, report_no ? report_no : "");
    sb_printf(&b, ",\n  \"selected_index\": %d", selected_index);
    sb_printf(&b, ",\n  \"project_count\": %d", project_count);
    sb_append(&b, ",\n  \"project_options\": [");
    for (int i = 0; i < project_count; i++) {
        if (i) sb_append(&b, ", ");
        json_escape_append(&b, projects && projects[i] ? projects[i] : "");
    }
    sb_append(&b, "]\n}\n");
    atomic_write_file(UI_STATUS_FILE, b.data ? b.data : "", b.len);
    sb_free(&b);
}

void wait_path(const char *path, const char *name, const char *log_file) {
    while (!path_exists(path)) {
        log_line(log_file, "waiting for %s %s ...", name ? name : "path", path);
        sleep(1);
    }
}

char *read_file_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

int write_file_all(const char *path, const void *data, size_t len) {
    ensure_parent_dir(path);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

int atomic_write_file(const char *path, const void *data, size_t len) {
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (write_file_all(tmp, data, len) != 0) return -1;
    return rename(tmp, path);
}

int move_to_dir(const char *src, const char *dst_dir, const char *name) {
    ensure_dir(dst_dir);
    char dst[1200];
    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, name);
    return rename(src, dst);
}

int list_first_json(const char *dir, char *out_name, size_t out_size) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    char best[512] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 5 && strcmp(e->d_name + n - 5, ".json") == 0) {
            if (!best[0] || strcmp(e->d_name, best) < 0)
                snprintf(best, sizeof(best), "%s", e->d_name);
        }
    }
    closedir(d);
    if (!best[0]) return 0;
    snprintf(out_name, out_size, "%s", best);
    return 1;
}

char *safe_name(const char *value, const char *fallback) {
    const char *s = (value && *value) ? value : fallback;
    if (!s || !*s) s = "unknown";
    char *out = calloc(strlen(s) + 1, 1);
    size_t j = 0;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out[j++] = (char)c;
        else out[j++] = '_';
    }
    while (j > 0 && (out[j - 1] == '.' || out[j - 1] == '_')) out[--j] = 0;
    if (!j) strcpy(out, fallback && *fallback ? fallback : "unknown");
    return out;
}

char *shell_quote(const char *s) {
    strbuf_t b;
    sb_init(&b);
    sb_append(&b, "'");
    for (; s && *s; s++) {
        if (*s == '\'') sb_append(&b, "'\\''");
        else sb_appendn(&b, s, 1);
    }
    sb_append(&b, "'");
    return b.data;
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
char *base64_encode(const unsigned char *src, size_t len) {
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = src[i] << 16;
        if (i + 1 < len) v |= src[i + 1] << 8;
        if (i + 2 < len) v |= src[i + 2];
        out[j++] = b64tab[(v >> 18) & 63];
        out[j++] = b64tab[(v >> 12) & 63];
        out[j++] = (i + 1 < len) ? b64tab[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < len) ? b64tab[v & 63] : '=';
    }
    out[j] = 0;
    return out;
}

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_OBJ, JV_ARR } jtype_t;
typedef struct { char *key; struct json_value *value; } jpair_t;
struct json_value {
    jtype_t type;
    char *s;
    double num;
    int boolean;
    jpair_t *pairs;
    int pair_count;
    struct json_value **items;
    int item_count;
};

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static json_value_t *jnew(jtype_t t) {
    json_value_t *v = calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

static char *parse_string_raw(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    strbuf_t b;
    sb_init(&b);
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == '"' || *p == '\\' || *p == '/') sb_appendn(&b, p, 1);
            else if (*p == 'b') sb_appendn(&b, "\b", 1);
            else if (*p == 'f') sb_appendn(&b, "\f", 1);
            else if (*p == 'n') sb_appendn(&b, "\n", 1);
            else if (*p == 'r') sb_appendn(&b, "\r", 1);
            else if (*p == 't') sb_appendn(&b, "\t", 1);
            else if (*p == 'u') {
                unsigned cp = 0;
                int ok = 1;
                for (int i = 0; i < 4; i++) {
                    char h = p[1 + i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else ok = 0;
                }
                if (ok) {
                    char u8[4];
                    if (cp < 0x80) {
                        u8[0] = (char)cp;
                        sb_appendn(&b, u8, 1);
                    } else if (cp < 0x800) {
                        u8[0] = (char)(0xc0 | (cp >> 6));
                        u8[1] = (char)(0x80 | (cp & 0x3f));
                        sb_appendn(&b, u8, 2);
                    } else {
                        u8[0] = (char)(0xe0 | (cp >> 12));
                        u8[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
                        u8[2] = (char)(0x80 | (cp & 0x3f));
                        sb_appendn(&b, u8, 3);
                    }
                    p += 4;
                }
            }
            if (*p) p++;
        } else {
            sb_appendn(&b, p++, 1);
        }
    }
    if (*p == '"') p++;
    *pp = p;
    return b.data ? b.data : strdup("");
}

static json_value_t *parse_value(const char **pp);

static json_value_t *parse_array(const char **pp) {
    const char *p = *pp + 1;
    json_value_t *v = jnew(JV_ARR);
    p = skip_ws(p);
    if (*p == ']') { *pp = p + 1; return v; }
    while (*p) {
        json_value_t *item = parse_value(&p);
        if (!item) break;
        v->items = realloc(v->items, sizeof(v->items[0]) * (v->item_count + 1));
        v->items[v->item_count++] = item;
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') { p++; break; }
    }
    *pp = p;
    return v;
}

static json_value_t *parse_object(const char **pp) {
    const char *p = *pp + 1;
    json_value_t *v = jnew(JV_OBJ);
    p = skip_ws(p);
    if (*p == '}') { *pp = p + 1; return v; }
    while (*p) {
        p = skip_ws(p);
        char *key = parse_string_raw(&p);
        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);
        json_value_t *val = parse_value(&p);
        if (!key || !val) { free(key); break; }
        v->pairs = realloc(v->pairs, sizeof(v->pairs[0]) * (v->pair_count + 1));
        v->pairs[v->pair_count].key = key;
        v->pairs[v->pair_count].value = val;
        v->pair_count++;
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
    }
    *pp = p;
    return v;
}

static json_value_t *parse_value(const char **pp) {
    const char *p = skip_ws(*pp);
    json_value_t *v = NULL;
    if (*p == '"') {
        v = jnew(JV_STR);
        v->s = parse_string_raw(&p);
    } else if (*p == '{') {
        v = parse_object(&p);
    } else if (*p == '[') {
        v = parse_array(&p);
    } else if (!strncmp(p, "true", 4)) {
        v = jnew(JV_BOOL); v->boolean = 1; p += 4;
    } else if (!strncmp(p, "false", 5)) {
        v = jnew(JV_BOOL); p += 5;
    } else if (!strncmp(p, "null", 4)) {
        v = jnew(JV_NULL); p += 4;
    } else {
        char *end = NULL;
        v = jnew(JV_NUM);
        v->num = strtod(p, &end);
        p = end && end != p ? end : p + 1;
    }
    *pp = p;
    return v;
}

json_value_t *json_parse_text(const char *text) {
    const char *p = text;
    return parse_value(&p);
}

json_value_t *json_parse_file(const char *path) {
    char *text = read_file_all(path, NULL);
    if (!text) return NULL;
    json_value_t *v = json_parse_text(text);
    free(text);
    return v;
}

void json_free(json_value_t *v) {
    if (!v) return;
    free(v->s);
    for (int i = 0; i < v->pair_count; i++) {
        free(v->pairs[i].key);
        json_free(v->pairs[i].value);
    }
    for (int i = 0; i < v->item_count; i++) json_free(v->items[i]);
    free(v->pairs);
    free(v->items);
    free(v);
}

json_value_t *json_obj_get(json_value_t *obj, const char *key) {
    if (!obj || obj->type != JV_OBJ) return NULL;
    for (int i = 0; i < obj->pair_count; i++)
        if (!strcmp(obj->pairs[i].key, key)) return obj->pairs[i].value;
    return NULL;
}

json_value_t *json_arr_get(json_value_t *arr, int index) {
    if (!arr || arr->type != JV_ARR || index < 0 || index >= arr->item_count) return NULL;
    return arr->items[index];
}

int json_arr_size(json_value_t *arr) {
    return arr && arr->type == JV_ARR ? arr->item_count : 0;
}

const char *json_value_string(json_value_t *v, const char *defval) {
    static char numbuf[64];
    if (!v) return defval;
    if (v->type == JV_STR) return v->s ? v->s : "";
    if (v->type == JV_NUM) {
        snprintf(numbuf, sizeof(numbuf), "%.15g", v->num);
        return numbuf;
    }
    if (v->type == JV_BOOL) return v->boolean ? "true" : "false";
    return defval;
}

const char *json_str(json_value_t *obj, const char *key, const char *defval) {
    return json_value_string(json_obj_get(obj, key), defval);
}

int json_int(json_value_t *obj, const char *key, int defval) {
    json_value_t *v = json_obj_get(obj, key);
    if (!v) return defval;
    if (v->type == JV_NUM) return (int)v->num;
    if (v->type == JV_STR) return atoi(v->s);
    return defval;
}

int json_bool(json_value_t *obj, const char *key, int defval) {
    json_value_t *v = json_obj_get(obj, key);
    if (!v) return defval;
    if (v->type == JV_BOOL) return v->boolean;
    if (v->type == JV_NUM) return v->num != 0;
    if (v->type == JV_STR) return !strcmp(v->s, "true") || !strcmp(v->s, "1");
    return defval;
}

int json_value_is_array(json_value_t *v) { return v && v->type == JV_ARR; }
int json_value_is_object(json_value_t *v) { return v && v->type == JV_OBJ; }

void json_escape_append(strbuf_t *b, const char *s) {
    sb_append(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { sb_appendn(b, "\\", 1); sb_appendn(b, (const char *)&c, 1); }
        else if (c == '\n') sb_append(b, "\\n");
        else if (c == '\r') sb_append(b, "\\r");
        else if (c == '\t') sb_append(b, "\\t");
        else sb_appendn(b, (const char *)&c, 1);
    }
    sb_append(b, "\"");
}
