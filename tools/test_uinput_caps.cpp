#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool writeEvent(int fd, unsigned short type, unsigned short code, int value) {
    input_event ev = {};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        std::fprintf(stderr, "write event failed: %s\n", std::strerror(errno));
        return false;
    }
    return true;
}

static bool syn(int fd) {
    return writeEvent(fd, EV_SYN, SYN_REPORT, 0);
}

static bool tapCapsLock(int fd) {
    return writeEvent(fd, EV_KEY, KEY_CAPSLOCK, 1) && syn(fd) &&
           writeEvent(fd, EV_KEY, KEY_CAPSLOCK, 0) && syn(fd);
}

int main(int argc, char **argv) {
    int taps = 1;
    int settleMs = 500;
    int betweenMs = 100;
    bool fullKeyboard = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--taps") == 0 && i + 1 < argc) {
            taps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--settle-ms") == 0 && i + 1 < argc) {
            settleMs = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--between-ms") == 0 && i + 1 < argc) {
            betweenMs = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--full-keyboard") == 0) {
            fullKeyboard = true;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--taps N] [--settle-ms N] [--between-ms N] [--full-keyboard]\n",
                         argv[0]);
            return 2;
        }
    }

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "open /dev/uinput failed: %s\n", std::strerror(errno));
        return 1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    if (fullKeyboard) {
        for (int key = 1; key < KEY_MAX; key++) {
            ioctl(fd, UI_SET_KEYBIT, key);
        }
    } else {
        ioctl(fd, UI_SET_KEYBIT, KEY_CAPSLOCK);
    }
    ioctl(fd, UI_SET_EVBIT, EV_REP);
    ioctl(fd, UI_SET_EVBIT, EV_LED);
    ioctl(fd, UI_SET_LEDBIT, LED_CAPSL);

    uinput_setup setup = {};
    std::strcpy(setup.name, fullKeyboard ? "Ainput full uinput keyboard test"
                                         : "Ainput uinput caps test");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1;
    setup.id.product = 0x1;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        std::fprintf(stderr, "UI_DEV_SETUP failed: %s\n", std::strerror(errno));
        close(fd);
        return 1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        std::fprintf(stderr, "UI_DEV_CREATE failed: %s\n", std::strerror(errno));
        close(fd);
        return 1;
    }

    std::fprintf(stderr, "created uinput device; settling %dms\n", settleMs);
    usleep(settleMs * 1000);

    for (int i = 0; i < taps; i++) {
        std::fprintf(stderr, "tap CapsLock %d/%d\n", i + 1, taps);
        if (!tapCapsLock(fd)) {
            ioctl(fd, UI_DEV_DESTROY);
            close(fd);
            return 1;
        }
        if (i + 1 < taps) usleep(betweenMs * 1000);
    }

    usleep(100 * 1000);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
