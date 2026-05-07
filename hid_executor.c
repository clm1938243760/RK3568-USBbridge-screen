#define _GNU_SOURCE
#include "usb_bridge_common.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FORM_QUEUE_DIR BASE_DIR "/form_queue"
#define FORM_DONE_DIR BASE_DIR "/form_done"
#define ERROR_DIR BASE_DIR "/error"
#define LOG_FILE BASE_DIR "/logs/hid_executor.log"
#define KEYBOARD_DEV "/dev/hidg0"
#define MOUSE_DEV "/dev/hidg1"
#define SCREEN_W 1920
#define SCREEN_H 1080
#define ABS_MAX 32767
#define KEY_CAPSLOCK 0x39

static int led_state = -1;
static pthread_mutex_t led_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { unsigned char code, mod; } keymap_t;
static keymap_t km[128];

static void init_keymap(void) {
    for (int i = 0; i < 26; i++) {
        km['a' + i] = (keymap_t){ (unsigned char)(0x04 + i), 0x00 };
        km['A' + i] = (keymap_t){ (unsigned char)(0x04 + i), 0x02 };
    }
    const char *digits = "1234567890";
    int codes[] = {0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27};
    for (int i = 0; i < 10; i++) km[(int)digits[i]] = (keymap_t){ (unsigned char)codes[i], 0 };
    const char *shifted = "!@#$%^&*()";
    for (int i = 0; i < 10; i++) km[(int)shifted[i]] = (keymap_t){ (unsigned char)codes[i], 0x02 };
    struct { char ch; int code; int mod; } rest[] = {
        {'\n',0x28,0},{'\t',0x2b,0},{' ',0x2c,0},{'-',0x2d,0},{'_',0x2d,2},
        {'=',0x2e,0},{'+',0x2e,2},{'[',0x2f,0},{'{',0x2f,2},{']',0x30,0},{'}',0x30,2},
        {'\\',0x31,0},{'|',0x31,2},{';',0x33,0},{':',0x33,2},{'\'',0x34,0},{'"',0x34,2},
        {'`',0x35,0},{'~',0x35,2},{',',0x36,0},{'<',0x36,2},{'.',0x37,0},{'>',0x37,2},
        {'/',0x38,0},{'?',0x38,2}
    };
    for (size_t i = 0; i < sizeof(rest)/sizeof(rest[0]); i++)
        km[(int)rest[i].ch] = (keymap_t){ (unsigned char)rest[i].code, (unsigned char)rest[i].mod };
}

static void send_key_fd(int fd, unsigned char mod, unsigned char code) {
    unsigned char down[8] = { mod, 0, code, 0, 0, 0, 0, 0 };
    unsigned char up[8] = { 0 };
    write(fd, down, sizeof(down));
    fsync(fd);
    usleep(10000);
    write(fd, up, sizeof(up));
    fsync(fd);
    usleep(10000);
}

static void press_key(unsigned char mod, unsigned char code) {
    wait_path(KEYBOARD_DEV, "keyboard", LOG_FILE);
    int fd = open(KEYBOARD_DEV, O_WRONLY);
    if (fd >= 0) {
        send_key_fd(fd, mod, code);
        close(fd);
    }
}

static void *led_reader(void *arg) {
    (void)arg;
    for (;;) {
        wait_path(KEYBOARD_DEV, "keyboard", LOG_FILE);
        int fd = open(KEYBOARD_DEV, O_RDONLY | O_NONBLOCK);
        if (fd < 0) { sleep(1); continue; }
        log_line(LOG_FILE, "keyboard led reader start");
        for (;;) {
            unsigned char b[8];
            ssize_t n = read(fd, b, sizeof(b));
            if (n > 0) {
                pthread_mutex_lock(&led_lock);
                led_state = b[0];
                pthread_mutex_unlock(&led_lock);
                log_line(LOG_FILE, "keyboard led state=0x%02x caps=%s", b[0], (b[0] & 2) ? "on" : "off");
            } else {
                usleep(50000);
            }
        }
        close(fd);
    }
    return NULL;
}

static int get_caps(void) {
    pthread_mutex_lock(&led_lock);
    int s = led_state;
    pthread_mutex_unlock(&led_lock);
    return s < 0 ? -1 : ((s & 2) != 0);
}

static int wait_caps(void) {
    long long end = now_ms() + 500;
    while (now_ms() < end) {
        int s = get_caps();
        if (s >= 0) return s;
        usleep(50000);
    }
    return get_caps();
}

static void ensure_caps(int target) {
    int old = wait_caps();
    if (old < 0 || old != target) {
        press_key(0, KEY_CAPSLOCK);
        usleep(200000);
    }
}

static int has_non_ascii(const char *s) {
    for (; s && *s; s++) if ((unsigned char)*s >= 128) return 1;
    return 0;
}

static int env_enabled(const char *name, int defval) {
    const char *v = getenv(name);
    if (!v || !*v) return defval;
    return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "FALSE") != 0;
}

static char *ascii_lower_copy(const char *text) {
    char *out = strdup(text ? text : "");
    if (!out) return NULL;
    for (char *p = out; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    return out;
}

static void type_text(const char *text, int enter) {
    wait_path(KEYBOARD_DEV, "keyboard", LOG_FILE);
    int fd = open(KEYBOARD_DEV, O_WRONLY);
    if (fd < 0) return;
    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
        if (*p >= 128 || km[*p].code == 0) {
            log_line(LOG_FILE, "WARN unsupported char skipped: 0x%02x", *p);
            continue;
        }
        send_key_fd(fd, km[*p].mod, km[*p].code);
    }
    if (enter) send_key_fd(fd, 0, 0x28);
    close(fd);
}

static void type_ascii_caps_guard(const char *text, int enter) {
    int old = wait_caps();
    ensure_caps(1);
    char *lower = ascii_lower_copy(text);
    type_text(lower ? lower : text, enter);
    free(lower);
    ensure_caps(old >= 0 ? old : 0);
}

static void select_all(void) { press_key(0x01, 0x04); usleep(100000); }
static void paste_clipboard(void) { press_key(0x01, 0x19); usleep(300000); }
static void open_run_dialog(void) { press_key(0x08, 0x15); usleep(600000); }

static int should_caps_type(const char *text, const char *field) {
    if (!text || !*text || has_non_ascii(text)) return 0;
    int has_alpha = 0, has_digit_after = 0;
    for (int i = 0; text[i]; i++) {
        if ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z')) has_alpha = 1;
        if (i > 0 && text[i] >= '0' && text[i] <= '9') has_digit_after = 1;
    }
    if (field && (!strcmp(field, "patient_id") || !strcmp(field, "report_no") || !strcmp(field, "his_exam_no")))
        return has_alpha;
    return ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')) && has_digit_after;
}

static void type_ascii_direct(const char *text, const char *field) {
    if (env_enabled("HID_FORCE_CAPS_ASCII", 1) || should_caps_type(text, field)) {
        int old = wait_caps();
        ensure_caps(1);
        char *lower = ascii_lower_copy(text);
        type_text(lower, 0);
        free(lower);
        ensure_caps(old >= 0 ? old : 0);
    } else {
        type_text(text, 0);
    }
}

static int px_abs_x(int x) {
    if (x < 0) x = 0; if (x >= SCREEN_W) x = SCREEN_W - 1;
    return x * ABS_MAX / SCREEN_W;
}

static int px_abs_y(int y) {
    if (y < 0) y = 0; if (y >= SCREEN_H) y = SCREEN_H - 1;
    return y * ABS_MAX / SCREEN_H;
}

static void mouse_abs_report(int button, int x, int y) {
    wait_path(MOUSE_DEV, "mouse", LOG_FILE);
    int ax = px_abs_x(x), ay = px_abs_y(y);
    unsigned char data[5] = { (unsigned char)button, ax & 255, (ax >> 8) & 255, ay & 255, (ay >> 8) & 255 };
    int fd = open(MOUSE_DEV, O_WRONLY);
    if (fd >= 0) {
        write(fd, data, sizeof(data));
        fsync(fd);
        close(fd);
    }
}

static void click_abs(int x, int y) {
    log_line(LOG_FILE, "mouse_click_abs x=%d y=%d button=left", x, y);
    mouse_abs_report(0, x, y); usleep(50000);
    mouse_abs_report(1, x, y); usleep(80000);
    mouse_abs_report(0, x, y); usleep(100000);
}

static unsigned utf8_next(const unsigned char **pp) {
    const unsigned char *p = *pp;
    unsigned c = *p++;
    if (c < 0x80) { *pp = p; return c; }
    if ((c & 0xe0) == 0xc0 && p[0]) { c = ((c & 0x1f) << 6) | (p[0] & 0x3f); p += 1; }
    else if ((c & 0xf0) == 0xe0 && p[0] && p[1]) { c = ((c & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f); p += 2; }
    else if ((c & 0xf8) == 0xf0 && p[0] && p[1] && p[2]) { c = ((c & 0x07) << 18) | ((p[0] & 0x3f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3; }
    *pp = p;
    return c;
}

static void paste_text_windows(const char *text, int x, int y) {
    strbuf_t expr, cmd;
    sb_init(&expr); sb_init(&cmd);
    const unsigned char *p = (const unsigned char *)text;
    int first = 1;
    while (p && *p) {
        unsigned cp = utf8_next(&p);
        if (!first) sb_append(&expr, "+");
        first = 0;
        sb_printf(&expr, "[char]%u", cp);
    }
    sb_printf(&cmd, "powershell -sta -nop -w hidden -c \"Set-Clipboard -Value (%s)\"", expr.data ? expr.data : "''");
    log_line(LOG_FILE, "paste_text_windows: %s", text ? text : "");
    open_run_dialog();
    select_all();
    type_ascii_caps_guard(cmd.data, 1);
    usleep(1500000);
    click_abs(x, y);
    usleep(200000);
    select_all();
    paste_clipboard();
    sb_free(&expr); sb_free(&cmd);
}

static void process_form_task(const char *path) {
    json_value_t *task = json_parse_file(path);
    if (!task) return;
    json_value_t *patient = json_obj_get(task, "patient");
    json_value_t *events = json_obj_get(task, "eventClassList");
    const char *patient_id = json_str(patient, "patient_id", "");
    const char *report_no = json_str(patient, "report_no", "");
    const char *scan_text = json_str(task, "scan_text", "");
    int event_count = json_arr_size(events);
    log_line(LOG_FILE, "form_fill start patient_id=%s patient_name=%s sex=%s age=%s",
        patient_id, json_str(patient, "patient_name", ""),
        json_str(patient, "sex", ""), json_str(patient, "age", ""));
    log_line(LOG_FILE, "form_fill task=%s events=%d", path, event_count);
    if (event_count <= 0) {
        log_line(LOG_FILE, "ERROR form_fill has no events, skip wait_print");
        ui_status_update("error", "Form task invalid", "No HID events generated", scan_text, patient_id, report_no);
        json_free(task);
        return;
    }
    ui_status_update("inputting", "姝ｅ湪鑷姩褰曞叆", "璇峰嬁鎿嶄綔閿洏榧犳爣", scan_text, patient_id, report_no);
    for (int i = 0; i < event_count; i++) {
        json_value_t *ev = json_arr_get(events, i);
        int click_type = json_int(ev, "clickType", -1);
        int x = json_int(ev, "x", 0), y = json_int(ev, "y", 0);
        if (click_type == 0) {
            click_abs(x, y);
            usleep(150000);
        } else if (click_type == 1) {
            const char *text = json_str(ev, "text", "");
            const char *field = json_str(ev, "field", "");
            log_line(LOG_FILE, "input field=%s text=%s", field, text);
            if (has_non_ascii(text)) paste_text_windows(text, x, y);
            else {
                click_abs(x, y); usleep(50000); select_all(); type_ascii_direct(text, field);
            }
            usleep(80000);
        } else if (click_type == 7) {
            json_value_t *cond = json_obj_get(ev, "condition");
            const char *field = json_str(cond, "field", "");
            const char *equals = json_str(cond, "equals", "");
            const char *actual = json_str(patient, field, "");
            if (!strcmp(actual, equals)) {
                log_line(LOG_FILE, "radio matched %s=%s click x=%d y=%d", field, equals, x, y);
                click_abs(x, y); usleep(150000);
            } else {
                log_line(LOG_FILE, "radio skip %s need=%s actual=%s", field, equals, actual);
            }
        } else {
            log_line(LOG_FILE, "WARN unknown clickType=%d index=%d", click_type, json_int(ev, "index", -1));
        }
    }
    log_line(LOG_FILE, "form_fill done");
    ui_status_update("wait_print", "褰曞叆瀹屾垚", "璇风‘璁や俊鎭苟绛夊緟鎵撳嵃鎶ュ憡", scan_text, patient_id, report_no);
    json_free(task);
}

static void consume_form_dir(void) {
    char name[512], src[1024], work[1024];
    if (!list_first_json(FORM_QUEUE_DIR, name, sizeof(name))) return;
    snprintf(src, sizeof(src), "%s/%s", FORM_QUEUE_DIR, name);
    snprintf(work, sizeof(work), "%s/%s.work", FORM_QUEUE_DIR, name);
    if (rename(src, work) != 0) return;
    process_form_task(work);
    if (move_to_dir(work, FORM_DONE_DIR, name) == 0) log_line(LOG_FILE, "done: %s", name);
    else {
        move_to_dir(work, ERROR_DIR, name);
        exit(1);
    }
}

int main(void) {
    init_keymap();
    ensure_dir(FORM_QUEUE_DIR); ensure_dir(FORM_DONE_DIR); ensure_dir(ERROR_DIR); ensure_parent_dir(LOG_FILE);
    log_line(LOG_FILE, "hid_executor start");
    ui_status_update("idle", "绛夊緟鎵爜", "璇蜂娇鐢ㄦ壂鐮佹灙鎵弿鎮ｈ€匢D", "", "", "");
    log_line(LOG_FILE, "keyboard=%s mouse=%s", KEYBOARD_DEV, MOUSE_DEV);
    wait_path(KEYBOARD_DEV, "keyboard", LOG_FILE);
    wait_path(MOUSE_DEV, "mouse", LOG_FILE);
    pthread_t t;
    pthread_create(&t, NULL, led_reader, NULL);
    pthread_detach(t);
    for (;;) {
        consume_form_dir();
        usleep(100000);
    }
}
