#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int emit_event(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == sizeof(ev) ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s /dev/input/eventX key_code\n", argv[0]);
        fprintf(stderr, "example: %s /dev/input/event0 106\n", argv[0]);
        return 2;
    }

    const char *dev = argv[1];
    int code = atoi(argv[2]);
    int fd = open(dev, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
        return 1;
    }

    if (emit_event(fd, EV_KEY, (unsigned short)code, 1) != 0 ||
        emit_event(fd, EV_KEY, (unsigned short)code, 0) != 0 ||
        emit_event(fd, EV_SYN, SYN_REPORT, 0) != 0) {
        fprintf(stderr, "write event failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
