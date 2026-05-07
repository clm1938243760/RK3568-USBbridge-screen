#define _GNU_SOURCE
#include "usb_bridge_common.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define BIN BASE_DIR "/bin"
#define LOG_DIR BASE_DIR "/logs"
#define BOOT_LOG LOG_DIR "/boot.log"

static const char *printer_dev(void) {
    const char *v = getenv("PRINTER_DEV");
    return v && *v ? v : "/dev/g_printer0";
}

static void boot_log(const char *fmt, ...) {
    ensure_dir(LOG_DIR);
    va_list ap;
    va_start(ap, fmt);
    char *msg = NULL;
    vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (!msg) return;
    FILE *f = fopen(BOOT_LOG, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", now_text(), msg);
        fclose(f);
    }
    free(msg);
}

static void ensure_all_dirs(void) {
    const char *dirs[] = {
        LOG_DIR, BASE_DIR "/queue", BASE_DIR "/form_queue", BASE_DIR "/done", BASE_DIR "/form_done",
        BASE_DIR "/error", BASE_DIR "/api_raw", BASE_DIR "/config", BASE_DIR "/state", BASE_DIR "/report_wait_queue",
        BASE_DIR "/report_wait_done", BASE_DIR "/report_print_queue", BASE_DIR "/report_pdf_queue",
        BASE_DIR "/report_uploaded", BASE_DIR "/report_error"
    };
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) ensure_dir(dirs[i]);
}

static int wait_file_timeout(const char *path, const char *name) {
    for (int i = 0; i < 60; i++) {
        if (path_exists(path)) {
            boot_log("%s ready: %s", name, path);
            return 0;
        }
        boot_log("waiting for %s: %s", name, path);
        sleep(1);
    }
    boot_log("ERROR timeout waiting for %s: %s", name, path);
    return -1;
}

static void stop_all(void) {
    boot_log("stop old processes");
    system("pkill -f '/root/usb_bridge/bin/scan_patient_capture' 2>/dev/null");
    system("pkill -f '/root/usb_bridge/bin/hid_executor' 2>/dev/null");
    system("pkill -f '/root/usb_bridge/bin/printer_capture' 2>/dev/null");
    system("pkill -f '/root/usb_bridge/bin/report_uploader' 2>/dev/null");
    system("pkill -f '/root/usb_bridge/bin/bridge_ui' 2>/dev/null");
    unlink("/tmp/scan_patient_capture.pid");
    unlink("/tmp/hid_executor.pid");
    unlink("/tmp/printer_capture.pid");
    unlink("/tmp/report_uploader.pid");
    unlink("/tmp/bridge_ui.pid");
}

static void supervise(const char *name, const char *prog, const char *out_log) {
    int fd = open(out_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    for (;;) {
        boot_log("%s run: %s", name, prog);
        pid_t child = fork();
        if (child == 0) {
            execl(prog, prog, (char *)NULL);
            _exit(127);
        }
        int st = 0;
        waitpid(child, &st, 0);
        boot_log("%s exited status=%d, restart in 2s", name, st);
        sleep(2);
    }
}

static void start_worker(const char *name, const char *prog, const char *out_log, const char *pid_file) {
    boot_log("start %s", name);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            close(nullfd);
        }
        supervise(name, prog, out_log);
        _exit(0);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    write_file_all(pid_file, buf, strlen(buf));
    boot_log("%s supervisor pid=%d", name, (int)pid);
}

static int start_all(void) {
    ensure_all_dirs();
    boot_log("S98usbbridge start");
    stop_all();
    if (wait_file_timeout("/dev/hidg0", "keyboard hid") != 0) return 1;
    if (wait_file_timeout("/dev/hidg1", "mouse hid") != 0) return 1;
    if (wait_file_timeout(printer_dev(), "usb printer") != 0) return 1;
    system("rm -f /root/usb_bridge/queue/*.json /root/usb_bridge/queue/*.work 2>/dev/null");

    setenv("PRINTER_DEV", printer_dev(), 1);
    setenv("PRINT_IDLE_TIMEOUT", getenv("PRINT_IDLE_TIMEOUT") ? getenv("PRINT_IDLE_TIMEOUT") : "2.0", 1);

    if (strcmp(getenv("BRIDGE_UI_DISABLE") ? getenv("BRIDGE_UI_DISABLE") : "0", "1") != 0)
        start_worker("bridge_ui", BIN "/bridge_ui", LOG_DIR "/bridge_ui_stdout.log", "/tmp/bridge_ui.pid");
    start_worker("hid_executor", BIN "/hid_executor", LOG_DIR "/hid_executor_stdout.log", "/tmp/hid_executor.pid");
    sleep(1);
    start_worker("scan_patient_capture", BIN "/scan_patient_capture", LOG_DIR "/scan_patient_capture_stdout.log", "/tmp/scan_patient_capture.pid");
    start_worker("printer_capture", BIN "/printer_capture", LOG_DIR "/printer_capture_stdout.log", "/tmp/printer_capture.pid");
    start_worker("report_uploader", BIN "/report_uploader", LOG_DIR "/report_uploader_stdout.log", "/tmp/report_uploader.pid");
    sleep(1);
    boot_log("S98usbbridge started");
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "start";
    ensure_all_dirs();
    if (!strcmp(cmd, "start")) return start_all();
    if (!strcmp(cmd, "stop")) { boot_log("S98usbbridge stop"); stop_all(); return 0; }
    if (!strcmp(cmd, "restart")) { boot_log("S98usbbridge restart"); stop_all(); sleep(1); return start_all(); }
    fprintf(stderr, "Usage: %s {start|stop|restart}\n", argv[0]);
    return 1;
}
