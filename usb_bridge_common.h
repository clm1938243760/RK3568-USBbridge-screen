#ifndef USB_BRIDGE_COMMON_H
#define USB_BRIDGE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define BASE_DIR "/root/usb_bridge"
#define UI_STATUS_FILE BASE_DIR "/state/ui_status.json"

typedef struct {
    char *data;
    size_t len;
} strbuf_t;

typedef struct json_value json_value_t;

void ensure_dir(const char *path);
void ensure_parent_dir(const char *path);
void log_line(const char *log_file, const char *fmt, ...);
void ui_status_update(const char *phase, const char *title, const char *message,
                      const char *scan_text, const char *patient_id, const char *report_no);
void ui_status_update_projects(const char *scan_text, const char *patient_id, const char *report_no,
                               int selected_index, int project_count, const char **projects);
int path_exists(const char *path);
void wait_path(const char *path, const char *name, const char *log_file);
char *read_file_all(const char *path, size_t *out_len);
int write_file_all(const char *path, const void *data, size_t len);
int atomic_write_file(const char *path, const void *data, size_t len);
int move_to_dir(const char *src, const char *dst_dir, const char *name);
int list_first_json(const char *dir, char *out_name, size_t out_size);
long long now_ms(void);
const char *now_text(void);
char *safe_name(const char *value, const char *fallback);
char *shell_quote(const char *s);
char *base64_encode(const unsigned char *src, size_t len);

json_value_t *json_parse_file(const char *path);
json_value_t *json_parse_text(const char *text);
void json_free(json_value_t *v);
json_value_t *json_obj_get(json_value_t *obj, const char *key);
json_value_t *json_arr_get(json_value_t *arr, int index);
int json_arr_size(json_value_t *arr);
const char *json_str(json_value_t *obj, const char *key, const char *defval);
int json_int(json_value_t *obj, const char *key, int defval);
int json_bool(json_value_t *obj, const char *key, int defval);
const char *json_value_string(json_value_t *v, const char *defval);
int json_value_is_array(json_value_t *v);
int json_value_is_object(json_value_t *v);
void json_escape_append(strbuf_t *b, const char *s);

void sb_init(strbuf_t *b);
void sb_free(strbuf_t *b);
void sb_append(strbuf_t *b, const char *s);
void sb_appendn(strbuf_t *b, const char *s, size_t n);
void sb_printf(strbuf_t *b, const char *fmt, ...);

#endif
