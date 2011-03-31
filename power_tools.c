/*
 * Copyright (c) 2010, NVIDIA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 *  * Neither the name of NVIDIA nor the names of its contributors may
 *    be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* needed for strdup() and asprintf() */
#define _GNU_SOURCE

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/hiddev.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>

#include "seaboard.h"

static char *prog_name;

struct state {
    char *hiddev_filename;
    int fd;
    int timestamps;
    int no_names;
};

#define outerr(fmt, args...) fprintf(stderr, "%s: " fmt "\n", prog_name, ## args)

#define perr(fmt, args...) outerr(fmt ": %s", ## args, strerror(errno))

/* Constants to identify the HW */
#define NVIDIA_VENDOR_ID 0x955
#define USB_DAC_PRODUCT_ID 0x9

static int is_usage_current(int usage) {
    /* Odd usages are current */
    return !!(usage & 1);
}

static int probe_from_usage(int usage) {
    /* Each probe covers two consecutive usages, so this gets the probe index */
    return usage >> 1;
}

static double scale_reading(int input, int usage) {
    if (is_usage_current(usage)) {
        static const double scale_factor = 3.3 / 1023. / 100.;
        double output = input * scale_factor;
        output /= seaboard_probes[probe_from_usage(usage)].resistance;
        return output;
    } else {
        static const double scale_factor = 3.3 / 1023. / 0.1708;
        double output = input * scale_factor;
        return output;
    }
}

static void print_data(struct state *s, double value, int usage,
                       uint64_t time) {
    const char *name = seaboard_probes[probe_from_usage(usage)].name;
    const char *type = is_usage_current(usage) ? "CURR" : "VOLT";

    if (s->timestamps) {
        printf("%" PRIu64 ": ", time);
    }

    if (s->no_names) {
        printf("%02u:%.16f\n", usage, value);
    } else {
        printf("%02u %8s %s %.16f\n", usage, name, type, value);
    }
}

static uint64_t gettime64() {
    struct timeval tv;
    if (gettimeofday(&tv, NULL)) {
        perr("gettimeofday failed");
        return 0;
    }

    return (uint64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}

#define USB_HID_ORDINAL_USAGE_PAGE 0xa

static int read_data(struct state *s) {
    struct hiddev_event events[1024];
    ssize_t num_read;

retry:
    while ((num_read = read(s->fd, &events, sizeof(events))) > 0) {
        int i;
        uint64_t time = gettime64();

        for (i = 0; i < num_read / sizeof(events[0]); i += 1) {
            double scaled_value;
            int usage_page = events[i].hid >> 16;
            int usage_code = (events[i].hid & 0xffff) - 1;

            /* All of the "data" reports are "ordinal", skip the rest */
            if (usage_page != USB_HID_ORDINAL_USAGE_PAGE)
                continue;

            scaled_value = scale_reading(events[i].value, usage_code);

            print_data(s, scaled_value, usage_code, time);
        }

        /* Without this, realtime graph may lag */
        fflush(NULL);
    }

    if (num_read < 0) {
        int err = errno;
        if (err == EINTR) {
            goto retry;
        }
        perr("Failed to read from USB device");
        return err;
    }

    return 0;
}

static int try_open_device(struct state *s, const char *filename) {
    int fd = -1;
    struct hiddev_devinfo dev_info = { 0 };
    int ret;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        ret = errno;
        perr("failed to open file '%s'", filename);
        goto fail;
    }

    /* Check if we're looking at a NVIDIA USB DAC device */
    ret = ioctl(fd, HIDIOCGDEVINFO, &dev_info);
    if (ret) {
        perr("Failed to read device info for %s", filename);
        goto fail;
    }

    if (dev_info.vendor != NVIDIA_VENDOR_ID ||
        dev_info.product != USB_DAC_PRODUCT_ID) {
        ret = EINVAL;
        goto fail;
    }

    s->fd = fd;
    return 0;

fail:
    if (fd >= 0) {
        close(fd);
    }

    return ret;
}

static int find_hiddev(struct state *s) {
    static char *dev_usb_dir = "/dev/usb";
    struct dirent *dirent;
    DIR *dir;

    /* If the user picked a filename, just use that */
    if (s->hiddev_filename)
        return try_open_device(s, s->hiddev_filename);

    /* Look for files /dev/usb/hiddev* */
    dir = opendir(dev_usb_dir);
    if (!dir) {
        int err = errno;
        perr("Failed to open %s", dev_usb_dir);
        return err;
    }

    while ((dirent = readdir(dir))) {
        char *filename = NULL;

#if defined(_DIRENT_HAVE_D_TYPE)
        if (dirent->d_type != DT_CHR)
            continue;
#endif

        if (asprintf(&filename, "%s/%s", dev_usb_dir, dirent->d_name) == -1)
            return ENOMEM;

        if (!try_open_device(s, filename)) {
            fprintf(stderr, "Found USB device at %s\n", filename);
            s->hiddev_filename = filename;
            return 0;
        }

        free(filename);
        filename = NULL;
    }

    return ENOENT;
}

static void print_help(void) {
    fprintf(stderr, "Usage: %s <options>\n\n", prog_name);
    fprintf(stderr, "Valid options:\n");
    fprintf(stderr, "--help, -h\n");
    fprintf(stderr, "    Print this help text and exit\n");
    fprintf(stderr, "--hiddev_file <path>, -f <path>\n");
    fprintf(stderr, "    Manually specify the hiddev device file to open\n");
    fprintf(stderr, "    Default: autodetect\n");
    fprintf(stderr, "--timestamps, -t\n");
    fprintf(stderr, "    Output timestamps. Default: off\n");
    fprintf(stderr, "--short, -s\n");
    fprintf(stderr, "    Don't print strings describing the readings (useful "
                    "for scripting)\n");
    fprintf(stderr, "    Default: print the strings\n");
}

static int parse_options(struct state *s, int argc, char **argv) {
    const struct option longopts[] = {
        { .name = "help",
          .val = 'h', },
        { .name = "hiddev_file",
          .has_arg = 1,
          .val = 'f', },
        { .name = "timestamps",
          .val = 't', },
        { .name = "short",
          .val = 's', },
    };
    const char *shortopts = "f:hts";
    char opt;

    while ((opt = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            s->hiddev_filename = strdup(optarg);
            if (!s->hiddev_filename) {
                return ENOMEM;
            }
            break;
        case 't':
            s->timestamps = 1;
            break;
        case 's':
            s->no_names = 1;
            break;
        case 'h':
            print_help();
            exit(0);
        case '?':
            return EINVAL;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    static struct state s;
    prog_name = argv[0];

    if (parse_options(&s, argc, argv)) {
        print_help();
        return 1;
    }

    if (find_hiddev(&s)) {
        fprintf(stderr, "%s: Failed to find USB device, aborting\n", prog_name);
        return 1;
    }

    if (read_data(&s)) {
        fprintf(stderr, "%s: Failed to read data, aborting\n", prog_name);
        return 1;
    }

    close(s.fd);

    return 0;
}
