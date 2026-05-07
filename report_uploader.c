#define _GNU_SOURCE
#include "api_config.h"
#include "usb_bridge_common.h"

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG_FILE BASE_DIR "/logs/report_uploader.log"
#define REPORT_PRINT_QUEUE_DIR BASE_DIR "/report_print_queue"
#define REPORT_PDF_QUEUE_DIR BASE_DIR "/report_pdf_queue"
#define REPORT_UPLOADED_DIR BASE_DIR "/report_uploaded"
#define REPORT_ERROR_DIR BASE_DIR "/report_error"

static const char *envdef(const char *k, const char *d) {
    const char *v = getenv(k);
    return v && *v ? v : d;
}

static void ensure_dirs(void) {
    ensure_parent_dir(LOG_FILE);
    ensure_dir(REPORT_PRINT_QUEUE_DIR);
    ensure_dir(REPORT_PDF_QUEUE_DIR);
    ensure_dir(REPORT_UPLOADED_DIR);
    ensure_dir(REPORT_ERROR_DIR);
}

static int next_print_task(char *work, size_t work_size, json_value_t **task) {
    char name[512], src[1024];
    if (!list_first_json(REPORT_PRINT_QUEUE_DIR, name, sizeof(name))) return 0;
    snprintf(src, sizeof(src), "%s/%s", REPORT_PRINT_QUEUE_DIR, name);
    snprintf(work, work_size, "%s/%s.work", REPORT_PRINT_QUEUE_DIR, name);
    if (rename(src, work) != 0) return 0;
    *task = json_parse_file(work);
    return *task != NULL;
}

static int convert_ps_to_pdf(json_value_t *task, char *pdf_path, size_t pdf_size) {
    const char *ps = json_str(task, "ps_file", "");
    if (!*ps || !path_exists(ps)) {
        log_line(LOG_FILE, "ps file not found: %s", ps);
        return -1;
    }
    const char *base = json_str(task, "base_name", "");
    char derived[512];
    if (!*base) {
        const char *p = strrchr(ps, '/');
        snprintf(derived, sizeof(derived), "%s", p ? p + 1 : ps);
        char *dot = strrchr(derived, '.');
        if (dot) *dot = 0;
        base = derived;
    }
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s/%s.pdf.tmp", REPORT_PDF_QUEUE_DIR, base);
    snprintf(pdf_path, pdf_size, "%s/%s.pdf", REPORT_PDF_QUEUE_DIR, base);

    char *gs = shell_quote(envdef("GS_BIN", "gs"));
    char *out = shell_quote(tmp);
    char *in = shell_quote(ps);
    strbuf_t cmd;
    sb_init(&cmd);
    sb_printf(&cmd, "%s -dBATCH -dNOPAUSE -dSAFER -sDEVICE=pdfwrite -sOutputFile=%s %s", gs, out, in);
    log_line(LOG_FILE, "convert ps to pdf: %s", cmd.data);
    int rc = system(cmd.data);
    free(gs); free(out); free(in); sb_free(&cmd);
    if (rc != 0) return -1;
    if (rename(tmp, pdf_path) != 0) return -1;
    log_line(LOG_FILE, "pdf created: %s", pdf_path);
    return 0;
}

static int upload_pdf(const char *pdf, json_value_t *task, const api_config_t *cfg) {
    const char *url = cfg->report_upload_url;
    if (!url || !*url) {
        log_line(LOG_FILE, "REPORT_UPLOAD_URL is not set in env or %s", API_CONFIG_FILE);
        return -1;
    }
    const char *field = cfg->report_upload_field_name;
    int timeout = cfg->report_upload_timeout;
    char form_file[1200];
    snprintf(form_file, sizeof(form_file), "%s=@%s;type=application/pdf", field, pdf);
    char header_pid[512], header_rno[512], header_his[512];
    snprintf(header_pid, sizeof(header_pid), "X-Patient-Id: %s", json_str(task, "patient_id", ""));
    snprintf(header_rno, sizeof(header_rno), "X-Report-No: %s", json_str(task, "report_no", ""));
    snprintf(header_his, sizeof(header_his), "X-His-Exam-No: %s", json_str(task, "his_exam_no", ""));
    char *qurl = shell_quote(url);
    char *qform_file = shell_quote(form_file);
    char *qhp = shell_quote(header_pid);
    char *qhr = shell_quote(header_rno);
    char *qhh = shell_quote(header_his);
    char *pid = shell_quote(json_str(task, "patient_id", ""));
    char *rno = shell_quote(json_str(task, "report_no", ""));
    char *his = shell_quote(json_str(task, "his_exam_no", ""));
    char *scan = shell_quote(json_str(task, "scan_text", ""));
    strbuf_t cmd;
    sb_init(&cmd);
    sb_printf(&cmd,
        "curl -fsS --max-time %d -A RK3568-USB-Bridge "
        "-H %s -H %s -H %s "
        "-F patient_id=%s -F report_no=%s -F his_exam_no=%s -F scan_text=%s -F %s %s",
        timeout, qhp, qhr, qhh, pid, rno, his, scan, qform_file, qurl);
    log_line(LOG_FILE, "upload pdf: %s", pdf);
    int rc = system(cmd.data);
    free(qurl); free(qform_file); free(qhp); free(qhr); free(qhh);
    free(pid); free(rno); free(his); free(scan); sb_free(&cmd);
    return rc == 0 ? 0 : -1;
}

static void move_clean(const char *path, const char *dir) {
    if (!path || !*path || !path_exists(path)) return;
    const char *name = strrchr(path, '/');
    char clean[512];
    snprintf(clean, sizeof(clean), "%s", name ? name + 1 : path);
    char *p = strstr(clean, ".work");
    if (p) *p = 0;
    move_to_dir(path, dir, clean);
}

int main(void) {
    ensure_dirs();
    api_config_t cfg;
    api_config_load(&cfg);
    log_line(LOG_FILE, "report_uploader start");
    api_config_log(&cfg, LOG_FILE);
    for (;;) {
        char work[1024], pdf[1024];
        json_value_t *task = NULL;
        if (!next_print_task(work, sizeof(work), &task)) {
            usleep(200000);
            continue;
        }
        api_config_load(&cfg);
        ui_status_update("converting", "正在生成PDF", "打印数据转换中",
            json_str(task, "scan_text", ""), json_str(task, "patient_id", ""), json_str(task, "report_no", ""));
        int converted = convert_ps_to_pdf(task, pdf, sizeof(pdf));
        if (converted == 0) {
            ui_status_update("uploading", "正在上传报告", "请保持网络连接",
                json_str(task, "scan_text", ""), json_str(task, "patient_id", ""), json_str(task, "report_no", ""));
        }
        if (converted == 0 && upload_pdf(pdf, task, &cfg) == 0) {
            ui_status_update("done", "上传完成", "可以继续扫描下一位患者",
                json_str(task, "scan_text", ""), json_str(task, "patient_id", ""), json_str(task, "report_no", ""));
            move_clean(work, REPORT_UPLOADED_DIR);
            move_clean(json_str(task, "ps_file", ""), REPORT_UPLOADED_DIR);
            move_clean(pdf, REPORT_UPLOADED_DIR);
            sleep(5);
            ui_status_update("idle", "等待扫码", "请使用扫码枪扫描患者ID", "", "", "");
        } else {
            ui_status_update("error", "报告上传失败", "请检查网络、PDF转换和接口配置",
                json_str(task, "scan_text", ""), json_str(task, "patient_id", ""), json_str(task, "report_no", ""));
            move_clean(work, REPORT_ERROR_DIR);
            log_line(LOG_FILE, "FATAL report upload failed, exit for supervisor restart");
            json_free(task);
            return 1;
        }
        json_free(task);
    }
}
