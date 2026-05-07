#define _GNU_SOURCE
#include "usb_bridge_common.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define G "/sys/kernel/config/usb_gadget/rockchip"

static void ulog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[S50usbdevice] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static int write_text(const char *path, const char *text) {
    return write_file_all(path, text, strlen(text));
}

static int write_bin(const char *path, const unsigned char *data, size_t len) {
    return write_file_all(path, data, len);
}

static int get_udc(char *out, size_t size) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(out, size, "%s", e->d_name);
        ok = 1;
        break;
    }
    closedir(d);
    return ok;
}

static void unbind_udc(void) {
    if (path_exists(G "/UDC")) {
        write_text(G "/UDC", "");
        sleep(1);
    }
}

static void remove_old_links(void) {
    DIR *d = opendir(G "/configs/b.1");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), G "/configs/b.1/%s", e->d_name);
        unlink(path);
    }
    closedir(d);
}

static int setup_gadget(void) {
    char udc[128];
    if (!get_udc(udc, sizeof(udc))) {
        ulog("ERROR: no UDC found");
        return 1;
    }
    if (!path_exists("/sys/kernel/config/usb_gadget"))
        system("mount -t configfs none /sys/kernel/config");
    ensure_dir(G);
    ulog("UDC=%s", udc);
    ulog("unbind old gadget");
    unbind_udc();
    ulog("remove old links");
    remove_old_links();

    ulog("set descriptors");
    write_text(G "/idVendor", "0x2207\n");
    write_text(G "/idProduct", "0x3568\n");
    write_text(G "/bcdUSB", "0x0200\n");
    write_text(G "/bcdDevice", "0x0100\n");
    ensure_dir(G "/strings/0x409");
    write_text(G "/strings/0x409/serialnumber", "RK3568BRIDGE001\n");
    write_text(G "/strings/0x409/manufacturer", "RK3568\n");
    write_text(G "/strings/0x409/product", "RK3568 HID Keyboard Mouse Printer Bridge\n");
    ensure_dir(G "/configs/b.1/strings/0x409");
    write_text(G "/configs/b.1/strings/0x409/configuration", "HID Keyboard + HID Mouse + USB Printer\n");
    write_text(G "/configs/b.1/MaxPower", "120\n");

    static const unsigned char keyboard_desc[] = {
        0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x03,0x95,0x05,0x75,0x01,
        0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x03,0x95,0x06,
        0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xc0
    };
    static const unsigned char mouse_desc[] = {
        0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,
        0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x03,
        0x05,0x01,0x09,0x30,0x09,0x31,0x16,0x00,0x00,0x26,0xff,0x7f,0x36,0x00,0x00,0x46,
        0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,0xc0,0xc0
    };

    ulog("setup HID keyboard: hid.usb0");
    ensure_dir(G "/functions/hid.usb0");
    write_text(G "/functions/hid.usb0/protocol", "1\n");
    write_text(G "/functions/hid.usb0/subclass", "1\n");
    write_text(G "/functions/hid.usb0/report_length", "8\n");
    write_bin(G "/functions/hid.usb0/report_desc", keyboard_desc, sizeof(keyboard_desc));

    ulog("setup HID mouse: hid.usb1");
    ensure_dir(G "/functions/hid.usb1");
    write_text(G "/functions/hid.usb1/protocol", "2\n");
    write_text(G "/functions/hid.usb1/subclass", "1\n");
    write_text(G "/functions/hid.usb1/report_length", "5\n");
    write_bin(G "/functions/hid.usb1/report_desc", mouse_desc, sizeof(mouse_desc));

    ulog("setup USB printer: printer.usb0");
    ensure_dir(G "/functions/printer.usb0");
    write_text(G "/functions/printer.usb0/q_len", "10\n");
    write_text(G "/functions/printer.usb0/pnp_string", "MFG:RK3568;MDL:Virtual Printer;CLS:PRINTER;\n");

    ulog("link keyboard + mouse + printer");
    symlink(G "/functions/hid.usb0", G "/configs/b.1/f_keyboard");
    symlink(G "/functions/hid.usb1", G "/configs/b.1/f_mouse");
    symlink(G "/functions/printer.usb0", G "/configs/b.1/f_printer");
    ulog("bind UDC");
    write_text(G "/UDC", udc);
    sleep(1);
    system("mdev -s 2>/dev/null || true");
    write_text("/tmp/.usb_config", "usb_hid_mouse_printer_en\n");
    ulog("done");
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "start";
    if (!strcmp(cmd, "start")) return setup_gadget();
    if (!strcmp(cmd, "stop")) { ulog("stop gadget"); unbind_udc(); return 0; }
    if (!strcmp(cmd, "restart")) { ulog("stop gadget"); unbind_udc(); sleep(1); return setup_gadget(); }
    fprintf(stderr, "Usage: %s {start|stop|restart}\n", argv[0]);
    return 1;
}
