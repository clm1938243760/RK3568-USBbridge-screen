#define _GNU_SOURCE
#include "api_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PATIENT_QUERY_URL "http://192.168.112.139:9061/api/client/getTJPatientInfo"
#define DEFAULT_REPORT_UPLOAD_URL "http://8.148.73.190:5000/upload"
#define DEFAULT_REPORT_UPLOAD_FIELD_NAME "file"

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

static void set_text(char *dst, size_t size, const char *value) {
    if (!dst || !size || !value || !*value) return;
    snprintf(dst, size, "%s", value);
}

static void apply_pair(api_config_t *cfg, const char *key, const char *value) {
    if (!key || !value || !*key || !*value) return;
    if (!strcmp(key, "PATIENT_QUERY_URL")) {
        set_text(cfg->patient_query_url, sizeof(cfg->patient_query_url), value);
    } else if (!strcmp(key, "REPORT_UPLOAD_URL")) {
        set_text(cfg->report_upload_url, sizeof(cfg->report_upload_url), value);
    } else if (!strcmp(key, "REPORT_UPLOAD_FIELD_NAME")) {
        set_text(cfg->report_upload_field_name, sizeof(cfg->report_upload_field_name), value);
    } else if (!strcmp(key, "PROJECT_INPUT_DEV")) {
        set_text(cfg->project_input_dev, sizeof(cfg->project_input_dev), value);
    } else if (!strcmp(key, "PATIENT_API_TIMEOUT")) {
        int v = atoi(value);
        if (v > 0) cfg->patient_api_timeout = v;
    } else if (!strcmp(key, "REPORT_UPLOAD_TIMEOUT")) {
        int v = atoi(value);
        if (v > 0) cfg->report_upload_timeout = v;
    } else if (!strcmp(key, "PROJECT_KEY_PREV")) {
        int v = atoi(value);
        if (v > 0) cfg->project_key_prev = v;
    } else if (!strcmp(key, "PROJECT_KEY_NEXT")) {
        int v = atoi(value);
        if (v > 0) cfg->project_key_next = v;
    } else if (!strcmp(key, "PROJECT_KEY_CONFIRM")) {
        int v = atoi(value);
        if (v > 0) cfg->project_key_confirm = v;
    }
}

static void load_file(api_config_t *cfg) {
    FILE *f = fopen(API_CONFIG_FILE, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s);
        char *value = trim(eq + 1);
        if ((*value == '"' || *value == '\'') && value[strlen(value) - 1] == *value) {
            value[strlen(value) - 1] = 0;
            value++;
        }
        apply_pair(cfg, key, value);
    }

    fclose(f);
}

static void load_env(api_config_t *cfg) {
    const char *v;
    v = getenv("PATIENT_QUERY_URL");
    if (v && *v) set_text(cfg->patient_query_url, sizeof(cfg->patient_query_url), v);
    v = getenv("REPORT_UPLOAD_URL");
    if (v && *v) set_text(cfg->report_upload_url, sizeof(cfg->report_upload_url), v);
    v = getenv("REPORT_UPLOAD_FIELD_NAME");
    if (v && *v) set_text(cfg->report_upload_field_name, sizeof(cfg->report_upload_field_name), v);
    v = getenv("PROJECT_INPUT_DEV");
    if (v && *v) set_text(cfg->project_input_dev, sizeof(cfg->project_input_dev), v);
    v = getenv("PATIENT_API_TIMEOUT");
    if (v && *v && atoi(v) > 0) cfg->patient_api_timeout = atoi(v);
    v = getenv("REPORT_UPLOAD_TIMEOUT");
    if (v && *v && atoi(v) > 0) cfg->report_upload_timeout = atoi(v);
    v = getenv("PROJECT_KEY_PREV");
    if (v && *v && atoi(v) > 0) cfg->project_key_prev = atoi(v);
    v = getenv("PROJECT_KEY_NEXT");
    if (v && *v && atoi(v) > 0) cfg->project_key_next = atoi(v);
    v = getenv("PROJECT_KEY_CONFIRM");
    if (v && *v && atoi(v) > 0) cfg->project_key_confirm = atoi(v);
}

void api_config_load(api_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    set_text(cfg->patient_query_url, sizeof(cfg->patient_query_url), DEFAULT_PATIENT_QUERY_URL);
    set_text(cfg->report_upload_url, sizeof(cfg->report_upload_url), DEFAULT_REPORT_UPLOAD_URL);
    set_text(cfg->report_upload_field_name, sizeof(cfg->report_upload_field_name), DEFAULT_REPORT_UPLOAD_FIELD_NAME);
    set_text(cfg->project_input_dev, sizeof(cfg->project_input_dev), "");
    cfg->patient_api_timeout = 10;
    cfg->report_upload_timeout = 30;
    cfg->project_key_prev = 105;      /* KEY_LEFT */
    cfg->project_key_next = 106;      /* KEY_RIGHT */
    cfg->project_key_confirm = 28;    /* KEY_ENTER */

    load_file(cfg);
    load_env(cfg);
}

void api_config_log(const api_config_t *cfg, const char *log_file) {
    log_line(log_file, "api config file=%s", API_CONFIG_FILE);
    log_line(log_file, "patient_query_url=%s timeout=%d",
        cfg->patient_query_url, cfg->patient_api_timeout);
    log_line(log_file, "report_upload_url=%s field=%s timeout=%d",
        cfg->report_upload_url, cfg->report_upload_field_name, cfg->report_upload_timeout);
    log_line(log_file, "project keys prev=%d next=%d confirm=%d",
        cfg->project_key_prev, cfg->project_key_next, cfg->project_key_confirm);
    log_line(log_file, "project_input_dev=%s",
        cfg->project_input_dev[0] ? cfg->project_input_dev : "<all>");
}
