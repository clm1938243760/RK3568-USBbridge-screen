#define _GNU_SOURCE
#include "usb_bridge_common.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_FILE BASE_DIR "/logs/printer_capture.log"
#define REPORT_WAIT_DIR BASE_DIR "/report_wait_queue"
#define REPORT_PRINT_QUEUE_DIR BASE_DIR "/report_print_queue"
#define REPORT_WAIT_DONE_DIR BASE_DIR "/report_wait_done"
#define REPORT_ERROR_DIR BASE_DIR "/report_error"
#define CHUNK_SIZE (64 * 1024)

static const char *printer_dev(void) {
    const char *v = getenv("PRINTER_DEV");
    return v && *v ? v : "/dev/g_printer0";
}

static double idle_timeout(void) {
    const char *v = getenv("PRINT_IDLE_TIMEOUT");
    return v && *v ? atof(v) : 2.0;
}

static int min_job_bytes(void) {
    const char *v = getenv("PRINT_MIN_JOB_BYTES");
    return v && *v ? atoi(v) : 128;
}

static void ensure_dirs(void) {
    ensure_parent_dir(LOG_FILE);
    ensure_dir(REPORT_WAIT_DIR);
    ensure_dir(REPORT_PRINT_QUEUE_DIR);
    ensure_dir(REPORT_WAIT_DONE_DIR);
    ensure_dir(REPORT_ERROR_DIR);
}

static int next_wait_task(char *work, size_t work_size, json_value_t **task) {
    char name[512], src[1024];
    if (!list_first_json(REPORT_WAIT_DIR, name, sizeof(name))) return 0;
    snprintf(src, sizeof(src), "%s/%s", REPORT_WAIT_DIR, name);
    snprintf(work, work_size, "%s/%s.work", REPORT_WAIT_DIR, name);
    if (rename(src, work) != 0) return 0;
    *task = json_parse_file(work);
    return *task != NULL;
}

static unsigned char *read_print_job(const char *dev, size_t *out_len) {
    wait_path(dev, "printer device", LOG_FILE);
    log_line(LOG_FILE, "waiting print job on %s", dev);

    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;

    unsigned char *buf = NULL;
    size_t len = 0;
    long long last = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    double idle = idle_timeout();

    for (;;) {
        int rc = poll(&pfd, 1, 100);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            unsigned char tmp[CHUNK_SIZE];
            ssize_t n = read(fd, tmp, sizeof(tmp));
            if (n > 0) {
                unsigned char *p = realloc(buf, len + (size_t)n);
                if (!p) break;
                buf = p;
                memcpy(buf + len, tmp, (size_t)n);
                len += (size_t)n;
                last = now_ms();
                continue;
            }
        }
        if (len > 0 && last && (now_ms() - last) >= (long long)(idle * 1000.0)) break;
    }
    close(fd);

    if ((int)len < min_job_bytes()) {
        log_line(LOG_FILE, "print job too small: %zu bytes", len);
        free(buf);
        return NULL;
    }
    log_line(LOG_FILE, "print job received: %zu bytes", len);
    *out_len = len;
    return buf;
}

static int write_print_queue(const unsigned char *job, size_t len, json_value_t *task) {
    json_value_t *patient = json_obj_get(task, "patient");
    const char *pid0 = json_str(task, "patient_id", json_str(patient, "patient_id", "unknown"));
    const char *rno0 = json_str(task, "report_no", json_str(patient, "report_no", pid0));
    char *pid = safe_name(pid0, "unknown");
    char *rno = safe_name(rno0, pid);
    long long ts = now_ms();

    char base[512], ps_tmp[1024], ps_path[1024], meta_path[1024], meta_tmp[1024];
    snprintf(base, sizeof(base), "%s_%s_%lld", pid, rno, ts);
    snprintf(ps_tmp, sizeof(ps_tmp), "%s/%s.ps.tmp", REPORT_PRINT_QUEUE_DIR, base);
    snprintf(ps_path, sizeof(ps_path), "%s/%s.ps", REPORT_PRINT_QUEUE_DIR, base);
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", REPORT_PRINT_QUEUE_DIR, base);
    snprintf(meta_tmp, sizeof(meta_tmp), "%s.tmp", meta_path);

    int ok = write_file_all(ps_tmp, job, len) == 0 && rename(ps_tmp, ps_path) == 0;
    if (ok) {
        strbuf_t m;
        sb_init(&m);
        sb_append(&m, "{\n");
        sb_printf(&m, "  \"status\": \"print_received\",\n  \"print_time\": ");
        json_escape_append(&m, now_text());
        sb_append(&m, ",\n  \"patient_id\": ");
        json_escape_append(&m, pid0);
        sb_append(&m, ",\n  \"report_no\": ");
        json_escape_append(&m, rno0);
        sb_append(&m, ",\n  \"his_exam_no\": ");
        json_escape_append(&m, json_str(task, "his_exam_no", json_str(patient, "his_exam_no", "")));
        sb_append(&m, ",\n  \"scan_text\": ");
        json_escape_append(&m, json_str(task, "scan_text", ""));
        sb_append(&m, ",\n  \"ps_file\": ");
        json_escape_append(&m, ps_path);
        sb_printf(&m, ",\n  \"base_name\": \"%s\",\n  \"bytes\": %zu\n}\n", base, len);
        ok = write_file_all(meta_tmp, m.data, m.len) == 0 && rename(meta_tmp, meta_path) == 0;
        sb_free(&m);
    }
    log_line(LOG_FILE, "report print task saved: %s", meta_path);
    free(pid);
    free(rno);
    return ok ? 0 : -1;
}

int main(void) {
    ensure_dirs();
    log_line(LOG_FILE, "printer_capture start printer=%s", printer_dev());
    for (;;) {
        char work[1024];
        json_value_t *task = NULL;
        if (!next_wait_task(work, sizeof(work), &task)) {
            usleep(200000);
            continue;
        }
        ui_status_update("wait_print", "等待打印", "请在电脑端打印当前患者报告",
            json_str(task, "scan_text", ""), json_str(task, "patient_id", ""), json_str(task, "report_no", ""));
        size_t len = 0;
        ui_status_update("printing", "正在接收打印数据", "请勿断开USB连接",
            json_str(task, "scan_text", ""), json_str(task, "patient_id", ""), json_str(task, "report_no", ""));
        unsigned char *job = read_print_job(printer_dev(), &len);
        if (job && write_print_queue(job, len, task) == 0) {
            ui_status_update("print_done", "打印数据已接收", "正在准备生成PDF并上传",
                json_str(task, "scan_text", ""), json_str(task, "patient_id", ""), json_str(task, "report_no", ""));
            char *name = strrchr(work, '/');
            name = name ? name + 1 : work;
            char clean[512];
            snprintf(clean, sizeof(clean), "%s", name);
            char *p = strstr(clean, ".work");
            if (p) *p = 0;
            move_to_dir(work, REPORT_WAIT_DONE_DIR, clean);
        } else {
            ui_status_update("error", "打印接收失败", "请检查打印机设备和USB连接",
                json_str(task, "scan_text", ""), json_str(task, "patient_id", ""), json_str(task, "report_no", ""));
            char *name = strrchr(work, '/');
            move_to_dir(work, REPORT_ERROR_DIR, name ? name + 1 : work);
            log_line(LOG_FILE, "FATAL printer capture failed, exit for supervisor restart");
            free(job);
            json_free(task);
            return 1;
        }
        free(job);
        json_free(task);
    }
}
