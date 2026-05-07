#ifndef API_CONFIG_H
#define API_CONFIG_H

#include <stddef.h>

#include "usb_bridge_common.h"

#define API_CONFIG_FILE BASE_DIR "/config/api.conf"

typedef struct {
    char patient_query_url[512];
    char report_upload_url[512];
    char report_upload_field_name[64];
    char project_input_dev[128];
    int patient_api_timeout;
    int report_upload_timeout;
    int project_key_prev;
    int project_key_next;
    int project_key_confirm;
} api_config_t;

void api_config_load(api_config_t *cfg);
void api_config_log(const api_config_t *cfg, const char *log_file);

#endif
