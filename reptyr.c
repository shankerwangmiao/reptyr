/*
 * Copyright (C) 2011 by Nelson Elhage
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <termios.h>
#include <signal.h>

#include "reptyr.h"
#include "reallocarray.h"
#include "platform/platform.h"

static int verbose = 0;

void _debug(const char *pfx, const char *msg, va_list ap) {

    if (pfx)
        fprintf(stderr, "%s", pfx);
    vfprintf(stderr, msg, ap);
    fprintf(stderr, "\n");
}

void die(const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    _debug("[!] ", msg, ap);
    va_end(ap);

    exit(1);
}

void debug(const char *msg, ...) {

    va_list ap;

    if (!verbose)
        return;

    va_start(ap, msg);
    _debug("[+] ", msg, ap);
    va_end(ap);
}

void error(const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    _debug("[-] ", msg, ap);
    va_end(ap);
}

void setup_raw(struct termios *save) {
    struct termios set;
    if (tcgetattr(0, save) < 0) {
        fprintf(stderr, "Unable to read terminal attributes: %m");
        return;
    }
    set = *save;
    cfmakeraw(&set);
    if (tcsetattr(0, TCSANOW, &set) < 0)
        die("Unable to set terminal attributes: %m");
}

void resize_pty(int pty) {
    struct winsize sz;
    if (ioctl(0, TIOCGWINSZ, &sz) < 0) {
        // provide fake size to workaround some problems
        struct winsize defaultsize = {30, 80, 640, 480};
        if (ioctl(pty, TIOCSWINSZ, &defaultsize) < 0) {
            fprintf(stderr, "Cannot set terminal size\n");
        }
        return;
    }
    ioctl(pty, TIOCSWINSZ, &sz);
}

int writeall(int fd, const void *buf, ssize_t count) {
    ssize_t rv;
    while (count > 0) {
        rv = write(fd, buf, count);
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            return rv;
        }
        count -= rv;
        buf += rv;
    }
    return 0;
}

volatile sig_atomic_t winch_happened = 0;

void do_winch(int signal) {
    winch_happened = 1;
}

void do_proxy(int pty) {
    char buf[4096];
    ssize_t count;
    fd_set set;
    sigset_t mask;
    sigset_t select_mask;
    struct sigaction sa;

    // Block WINCH while we're outside the select, but unblock it
    // while we're inside:
    sigemptyset(&mask);
    sigaddset(&mask, SIGWINCH);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        fprintf(stderr, "sigprocmask: %m");
        return;
    }
    sa.sa_handler = do_winch;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);
    resize_pty(pty);

    while (1) {
        if (winch_happened) {
            winch_happened = 0;
            resize_pty(pty);
        }
        FD_ZERO(&set);
        FD_SET(0, &set);
        FD_SET(pty, &set);
        sigemptyset(&select_mask);
        if (pselect(pty + 1, &set, NULL, NULL, NULL, &select_mask) < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "select: %m");
            return;
        }
        if (FD_ISSET(0, &set)) {
            count = read(0, buf, sizeof buf);
            if (count < 0)
                return;
            writeall(pty, buf, count);
        }
        if (FD_ISSET(pty, &set)) {
            count = read(pty, buf, sizeof buf);
            if (count <= 0)
                return;
            writeall(1, buf, count);
        }
    }
}

void usage(char *me) {
    fprintf(stderr, "Usage: %s [-s] PID\n", me);
    fprintf(stderr, "       %s -l|-L [COMMAND [ARGS]]\n", me);
    fprintf(stderr, "  -l    Create a new pty pair and print the name of the slave.\n");
    fprintf(stderr, "           if there are command-line arguments after -l\n");
    fprintf(stderr, "           they are executed with REPTYR_PTY set to path of pty.\n");
    fprintf(stderr, "  -L    Like '-l', but also redirect the child's stdio to the slave.\n");
    fprintf(stderr, "  -s    Attach fds 0-2 on the target, even if it is not attached to a tty.\n");
    fprintf(stderr, "  -T    Steal the entire terminal session of the target.\n");
    fprintf(stderr, "           [experimental] May be more reliable, and will attach all\n");
    fprintf(stderr, "           processes running on the terminal.\n");
    fprintf(stderr, "  -h    Print this help message and exit.\n");
    fprintf(stderr, "  -v    Print the version number and exit.\n");
    fprintf(stderr, "  -V    Print verbose debug output.\n");
}

int main(int argc, char **argv) {
    struct termios saved_termios;
    int pty;
    int opt;
    int err;
    int do_attach = 1;
    int force_stdio = 0;
    int do_steal = 0;
    int unattached_script_redirection = 0;

    while ((opt = getopt(argc, argv, "hlLsTvV")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;
        case 'l':
            do_attach = 0;
            break;
        case 'L':
            do_attach = 0;
            unattached_script_redirection = 1;
            break;
        case 's':
            force_stdio = 1;
            break;
        case 'T':
            do_steal = 1;
            break;
        case 'v':
            printf("This is reptyr version %s.\n", REPTYR_VERSION);
            printf(" by Nelson Elhage <nelhage@nelhage.com>\n");
            printf("http://github.com/nelhage/reptyr/\n");
            return 0;
        case 'V':
            verbose = 1;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
        if (opt == 'l' || opt == 'L') break; // the rest is a command line
    }

    if (do_attach && optind >= argc) {
        fprintf(stderr, "%s: No pid specified to attach\n", argv[0]);
        usage(argv[0]);
        return 1;
    }

    if (!do_steal) {
        if ((pty = get_pt()) < 0)
            die("Unable to allocate a new pseudo-terminal: %m");
        if (unlockpt(pty) < 0)
            die("Unable to unlockpt: %m");
        if (grantpt(pty) < 0)
            die("Unable to grantpt: %m");
    }

    if (do_attach) {
        char *endptr = NULL;
        errno = 0;
        long t = strtol(argv[optind], &endptr, 10);
        if (errno == ERANGE)
            die("Invalid pid: %m");
        if (*endptr)
            die("Invalid pid: must be integer");
        /* check for overflow/underflow */
        pid_t child = (pid_t)t;
        if (child < t || t < 1) /* pids can't be < 1, so no *real* underflow check */
            die("Invalid pid: %s", strerror(ERANGE));

        if (do_steal) {
            err = steal_pty(child, &pty);
        } else {
            err = attach_child(child, ptsname(pty), force_stdio);
        }
        if (err) {
            fprintf(stderr, "Unable to attach to pid %d: %s\n", child, strerror(err));
            if (err == EPERM) {
                check_ptrace_scope();
            }
            return 1;
        }
    } else {
        printf("Opened a new pty: %s\n", ptsname(pty));
        fflush(stdout);
        if (argc > 2) {
            if (!fork()) {
                setenv("REPTYR_PTY", ptsname(pty), 1);
                if (unattached_script_redirection) {
                    int f;
                    setpgid(0, getppid());
                    setsid();
                    f = open(ptsname(pty), O_RDONLY, 0);
                    dup2(f, 0);
                    close(f);
                    f = open(ptsname(pty), O_WRONLY, 0);
                    dup2(f, 1);
                    dup2(f, 2);
                    close(f);
                }
                close(pty);
                execvp(argv[2], argv + 2);
                exit(1);
            }
        }
    }

    setup_raw(&saved_termios);
    do_proxy(pty);
    do {
        errno = 0;
        if (tcsetattr(0, TCSANOW, &saved_termios) && errno != EINTR)
            die("Unable to tcsetattr: %m");
    } while (errno == EINTR);

    return 0;
}
