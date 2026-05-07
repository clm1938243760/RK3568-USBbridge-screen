#define _GNU_SOURCE
#include "usb_bridge_common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_FILE BASE_DIR "/logs/bridge_ui.log"

typedef struct {
    int fd;
    unsigned char *mem;
    unsigned char *shadow;
    long bytes;
    int use_mmap;
    int w;
    int h;
    int phys_w;
    int phys_h;
    int view_x;
    int view_y;
    int scale;
    int stride;
    int bpp;
} fb_t;

typedef struct {
    const char *title;
    const char *message;
    uint32_t color;
} phase_view_t;

static int refresh_ms(void) {
    const char *v = getenv("BRIDGE_UI_REFRESH_MS");
    int n = v && *v ? atoi(v) : 180;
    if (n < 80) n = 80;
    if (n > 1000) n = 1000;
    return n;
}

static int count_json(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 5 && !strcmp(e->d_name + len - 5, ".json")) n++;
    }
    closedir(d);
    return n;
}

static int fb_open(fb_t *fb) {
    memset(fb, 0, sizeof(*fb));
    const char *dev = getenv("BRIDGE_UI_FB");
    if (!dev || !*dev) dev = "/dev/fb0";

    fb->fd = open(dev, O_RDWR);
    if (fb->fd < 0) {
        log_line(LOG_FILE, "open framebuffer failed: %s", dev);
        return -1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        log_line(LOG_FILE, "read framebuffer info failed");
        close(fb->fd);
        return -1;
    }

    fb->phys_w = (int)var.xres;
    fb->phys_h = (int)var.yres;
    fb->w = 480;
    fb->h = 320;
    fb->bpp = (int)var.bits_per_pixel;
    fb->stride = (int)fix.line_length;
    fb->bytes = (long)fix.smem_len;
    long visible_bytes = (long)fb->stride * (long)fb->phys_h;
    if (fb->bytes < visible_bytes) fb->bytes = visible_bytes;
    fb->scale = fb->phys_w / fb->w;
    int sy = fb->phys_h / fb->h;
    if (sy > 0 && (fb->scale == 0 || sy < fb->scale)) fb->scale = sy;
    if (fb->scale < 1) fb->scale = 1;
    fb->view_x = (fb->phys_w - fb->w * fb->scale) / 2;
    fb->view_y = (fb->phys_h - fb->h * fb->scale) / 2;
    if (fb->view_x < 0) fb->view_x = 0;
    if (fb->view_y < 0) fb->view_y = 0;

    const char *use_mmap = getenv("BRIDGE_UI_USE_MMAP");
    if (use_mmap && atoi(use_mmap) != 0) {
        fb->mem = mmap(NULL, fb->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    } else {
        fb->mem = MAP_FAILED;
        errno = 0;
    }

    if (fb->mem == MAP_FAILED) {
        if (use_mmap && atoi(use_mmap) != 0)
            log_line(LOG_FILE, "mmap framebuffer failed: %s, fallback to write()", strerror(errno));
        else
            log_line(LOG_FILE, "mmap framebuffer disabled, use write()");
        fb->mem = calloc(1, (size_t)visible_bytes);
        if (!fb->mem) {
            close(fb->fd);
            return -1;
        }
        fb->shadow = fb->mem;
        fb->bytes = visible_bytes;
        fb->use_mmap = 0;
    } else {
        fb->use_mmap = 1;
    }

    log_line(LOG_FILE, "framebuffer=%s physical=%dx%d logical=%dx%d scale=%d offset=%d,%d bpp=%d stride=%d",
        dev, fb->phys_w, fb->phys_h, fb->w, fb->h, fb->scale, fb->view_x, fb->view_y,
        fb->bpp, fb->stride);
    return 0;
}

static void fb_flush(fb_t *fb) {
    if (fb->use_mmap) return;
    lseek(fb->fd, 0, SEEK_SET);
    ssize_t n = write(fb->fd, fb->shadow, (size_t)fb->bytes);
    if (n < 0) log_line(LOG_FILE, "write framebuffer failed: %s", strerror(errno));
}

static uint16_t rgb565(uint32_t c) {
    unsigned r = (c >> 16) & 0xff;
    unsigned g = (c >> 8) & 0xff;
    unsigned b = c & 0xff;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void put_phys_px(fb_t *fb, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= fb->phys_w || y >= fb->phys_h) return;
    unsigned char *p = fb->mem + y * fb->stride + x * (fb->bpp / 8);
    if (fb->bpp == 16) {
        *(uint16_t *)p = rgb565(c);
    } else if (fb->bpp == 24) {
        p[0] = c & 0xff;
        p[1] = (c >> 8) & 0xff;
        p[2] = (c >> 16) & 0xff;
    } else {
        p[0] = c & 0xff;
        p[1] = (c >> 8) & 0xff;
        p[2] = (c >> 16) & 0xff;
        p[3] = 0xff;
    }
}

static void put_px(fb_t *fb, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return;
    int px = fb->view_x + x * fb->scale;
    int py = fb->view_y + y * fb->scale;
    for (int yy = 0; yy < fb->scale; yy++)
        for (int xx = 0; xx < fb->scale; xx++)
            put_phys_px(fb, px + xx, py + yy, c);
}

static void fill_phys_rect(fb_t *fb, int x, int y, int w, int h, uint32_t c) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_phys_px(fb, xx, yy, c);
}

static void fill_rect(fb_t *fb, int x, int y, int w, int h, uint32_t c) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_px(fb, xx, yy, c);
}

static void stroke_rect(fb_t *fb, int x, int y, int w, int h, uint32_t c) {
    fill_rect(fb, x, y, w, 2, c);
    fill_rect(fb, x, y + h - 2, w, 2, c);
    fill_rect(fb, x, y, 2, h, c);
    fill_rect(fb, x + w - 2, y, 2, h, c);
}

static const uint8_t *glyph(char ch) {
    static const uint8_t sp[7] = {0,0,0,0,0,0,0};
    static const uint8_t q[7] = {0x0e,0x11,0x02,0x04,0x04,0x00,0x04};
    static const uint8_t dot[7] = {0,0,0,0,0,0x06,0x06};
    static const uint8_t dash[7] = {0,0,0,0x1f,0,0,0};
    static const uint8_t colon[7] = {0,0x06,0x06,0,0x06,0x06,0};
    static const uint8_t slash[7] = {0x01,0x02,0x04,0x08,0x10,0,0};
    static const uint8_t nums[10][7] = {
        {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},
        {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
        {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
        {0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
        {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
        {0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
        {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e},
        {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
        {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}
    };
    static const uint8_t letters[26][7] = {
        {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},
        {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
        {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e},
        {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
        {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},
        {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
        {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f},
        {0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
        {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e},
        {0x07,0x02,0x02,0x02,0x12,0x12,0x0c},
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
        {0x11,0x1b,0x15,0x15,0x11,0x11,0x11},
        {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
        {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
        {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
        {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},
        {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
        {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},
        {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
        {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x1b,0x11},
        {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
        {0x11,0x11,0x0a,0x04,0x04,0x04,0x04},
        {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}
    };
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'];
    if (ch >= '0' && ch <= '9') return nums[ch - '0'];
    if (ch == '.') return dot;
    if (ch == '-') return dash;
    if (ch == ':') return colon;
    if (ch == '/') return slash;
    if (ch == ' ') return sp;
    return q;
}

static void draw_char(fb_t *fb, int x, int y, char ch, uint32_t color, int scale) {
    const uint8_t *g = glyph(ch);
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (g[row] & (1 << (4 - col)))
                fill_rect(fb, x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void draw_text(fb_t *fb, int x, int y, const char *s, uint32_t color, int scale) {
    int cx = x;
    for (; s && *s; s++) {
        if (*s == '\n') {
            y += 9 * scale;
            cx = x;
            continue;
        }
        draw_char(fb, cx, y, *s, color, scale);
        cx += 6 * scale;
        if (cx > fb->w - 10) break;
    }
}

static void draw_center(fb_t *fb, int y, const char *s, uint32_t color, int scale) {
    int len = (int)strlen(s ? s : "");
    int w = len * 6 * scale;
    int x = (fb->w - w) / 2;
    if (x < 4) x = 4;
    draw_text(fb, x, y, s, color, scale);
}

static phase_view_t phase_view(const char *phase) {
    if (!phase || !*phase || !strcmp(phase, "idle"))
        return (phase_view_t){"WAIT SCAN", "SERVICE STARTED", 0x16a34a};
    if (!strcmp(phase, "scan"))
        return (phase_view_t){"QUERY PATIENT", "API ACCESSING", 0x0284c7};
    if (!strcmp(phase, "ready_input"))
        return (phase_view_t){"READY INPUT", "DO NOT TOUCH", 0xf59e0b};
    if (!strcmp(phase, "inputting"))
        return (phase_view_t){"INPUTTING", "NO MOUSE KEYBOARD", 0xf59e0b};
    if (!strcmp(phase, "wait_print"))
        return (phase_view_t){"INPUT DONE", "WAIT PRINT REPORT", 0x10b981};
    if (!strcmp(phase, "printing"))
        return (phase_view_t){"RECEIVE PRINT", "USB PRINT DATA", 0x8b5cf6};
    if (!strcmp(phase, "print_done"))
        return (phase_view_t){"PRINT RECEIVED", "MAKE PDF NEXT", 0x8b5cf6};
    if (!strcmp(phase, "converting"))
        return (phase_view_t){"MAKE PDF", "GS CONVERTING", 0x6366f1};
    if (!strcmp(phase, "uploading"))
        return (phase_view_t){"UPLOADING", "REPORT TO CLOUD", 0x0ea5e9};
    if (!strcmp(phase, "done"))
        return (phase_view_t){"UPLOAD DONE", "BACK IN 5 SEC", 0x22c55e};
    if (!strcmp(phase, "error"))
        return (phase_view_t){"FAILED", "SCAN AGAIN", 0xef4444};
    return (phase_view_t){"WORKING", "PLEASE WAIT", 0x06b6d4};
}

static int phase_progress(const char *phase, int tick) {
    if (!phase || !*phase || !strcmp(phase, "idle")) return 5;
    if (!strcmp(phase, "scan")) return 15 + (tick % 10);
    if (!strcmp(phase, "ready_input")) return 25;
    if (!strcmp(phase, "inputting")) return 35 + (tick % 18);
    if (!strcmp(phase, "wait_print")) return 58;
    if (!strcmp(phase, "printing")) return 68 + (tick % 10);
    if (!strcmp(phase, "print_done")) return 74;
    if (!strcmp(phase, "converting")) return 82 + (tick % 6);
    if (!strcmp(phase, "uploading")) return 90 + (tick % 8);
    if (!strcmp(phase, "done") || !strcmp(phase, "error")) return 100;
    return 20 + (tick % 40);
}

static void draw_bar(fb_t *fb, int x, int y, int w, int h, int progress, uint32_t color) {
    stroke_rect(fb, x, y, w, h, 0x334155);
    int fill = (w - 8) * progress / 100;
    fill_rect(fb, x + 4, y + 4, fill, h - 8, color);
}

static void render(fb_t *fb, int tick) {
    json_value_t *st = json_parse_file(UI_STATUS_FILE);
    const char *phase = json_str(st, "phase", "idle");
    const char *scan = json_str(st, "scan_text", "");
    const char *pid = json_str(st, "patient_id", "");
    const char *rno = json_str(st, "report_no", "");
    const char *time = json_str(st, "time", "");
    phase_view_t view = phase_view(phase);
    int progress = phase_progress(phase, tick);

    fill_phys_rect(fb, 0, 0, fb->phys_w, fb->phys_h, 0x020617);
    fill_rect(fb, 0, 0, fb->w, fb->h, 0x0f172a);
    fill_rect(fb, 0, 0, fb->w, 46, 0x1e3a8a);
    draw_text(fb, 14, 14, "RK3568 USB BRIDGE", 0xffffff, 2);

    fill_rect(fb, 18, 62, fb->w - 36, 98, 0x111827);
    stroke_rect(fb, 18, 62, fb->w - 36, 98, view.color);
    draw_center(fb, 82, view.title, view.color, 4);
    draw_center(fb, 128, view.message, 0xe5e7eb, 2);

    draw_bar(fb, 24, 178, fb->w - 48, 26, progress, view.color);

    draw_text(fb, 24, 222, "SCAN:", 0x94a3b8, 2);
    draw_text(fb, 112, 222, *scan ? scan : "-", 0xffffff, 2);
    draw_text(fb, 24, 246, "PATIENT:", 0x94a3b8, 2);
    draw_text(fb, 136, 246, *pid ? pid : "-", 0xffffff, 2);
    draw_text(fb, 24, 270, "REPORT:", 0x94a3b8, 2);
    draw_text(fb, 124, 270, *rno ? rno : "-", 0xffffff, 2);

    char q[128];
    snprintf(q, sizeof(q), "Q F%d P%d U%d E%d",
        count_json(BASE_DIR "/form_queue"),
        count_json(BASE_DIR "/report_wait_queue"),
        count_json(BASE_DIR "/report_print_queue"),
        count_json(BASE_DIR "/error") + count_json(BASE_DIR "/report_error"));
    draw_text(fb, 24, fb->h - 24, q, 0xcbd5e1, 1);
    draw_text(fb, fb->w - 150, fb->h - 24, *time ? time + (strlen(time) > 8 ? 11 : 0) : "-", 0xcbd5e1, 1);

    json_free(st);
    fb_flush(fb);
}

int main(void) {
    ensure_parent_dir(LOG_FILE);
    ensure_parent_dir(UI_STATUS_FILE);
    if (!path_exists(UI_STATUS_FILE))
        ui_status_update("idle", "等待扫码", "请使用扫码枪扫描患者ID", "", "", "");

    fb_t fb;
    if (fb_open(&fb) != 0) {
        log_line(LOG_FILE, "bridge_ui cannot start without framebuffer");
        return 1;
    }

    for (int tick = 0;; tick++) {
        render(&fb, tick);
        usleep(refresh_ms() * 1000);
    }
}
