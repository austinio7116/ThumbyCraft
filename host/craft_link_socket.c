/*
 * ThumbyCraft — 2-player link, host (SDL2/Linux) side.
 *
 * The host twin of the device's USB dual-role transport, mirroring
 * Mote's emulator link: a Unix stream socket at CRAFT_LINK_SOCK
 * (default /tmp/thumbycraft_link.sock). Discovery mirrors the device
 * role flip — try to connect() to a waiting peer; if nobody's there,
 * bind+listen and wait. Everything is non-blocking and pumped from
 * craft_link_task() once per frame, so two host instances started in
 * any order pair within a frame or two.
 */
#include "craft_link.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int  s_state;           /* CRAFT_LINK_* */
static int  s_fd = -1;         /* the connected pipe */
static int  s_listen = -1;     /* listener while waiting */
static int  s_bound;           /* we own the socket file (unlink on stop) */
static char s_path[108];

static void sock_path(struct sockaddr_un *a) {
    if (!s_path[0]) {
        const char *p = getenv("CRAFT_LINK_SOCK");
        snprintf(s_path, sizeof s_path, "%s", p ? p : "/tmp/thumbycraft_link.sock");
    }
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    snprintf(a->sun_path, sizeof a->sun_path, "%s", s_path);
}

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void close_conn(void) {
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
}

int craft_link_active(void) { return s_state != CRAFT_LINK_OFF; }

void craft_link_start(void) {
    if (s_state != CRAFT_LINK_OFF) return;
    s_state = CRAFT_LINK_SEARCHING;
}

void craft_link_stop(void) {
    close_conn();
    if (s_listen >= 0) { close(s_listen); s_listen = -1; }
    if (s_bound) { unlink(s_path); s_bound = 0; }
    s_state = CRAFT_LINK_OFF;
}

void craft_link_task(void) {
    if (s_state == CRAFT_LINK_OFF) return;

    if (s_state == CRAFT_LINK_CONNECTED) {
        /* Detect peer loss via EOF. */
        char probe;
        ssize_t r = recv(s_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) {
            close_conn();
            s_state = CRAFT_LINK_SEARCHING;
        }
        return;
    }

    /* SEARCHING. Listener side: poll accept. */
    if (s_listen >= 0) {
        int c = accept(s_listen, NULL, NULL);
        if (c >= 0) {
            set_nonblock(c);
            s_fd = c;
            s_state = CRAFT_LINK_CONNECTED;
        }
        return;
    }

    /* No role yet: try to connect to a waiting peer, else become the
     * listener. */
    struct sockaddr_un addr; sock_path(&addr);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        set_nonblock(fd);
        s_fd = fd;
        s_state = CRAFT_LINK_CONNECTED;
        return;
    }
    close(fd);
    /* connect refused/absent: claim the listener role. A stale socket
     * file from a crashed run would make bind fail forever, so clear it
     * first — but only when connect() said nobody is home. */
    if (errno == ECONNREFUSED) unlink(addr.sun_path);
    int lf = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lf < 0) return;
    if (bind(lf, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(lf, 1) == 0) {
        set_nonblock(lf);
        s_listen = lf;
        s_bound = 1;
    } else {
        close(lf);   /* EADDRINUSE: the peer just bound — connect() next frame */
    }
}

int craft_link_status(void) { return s_state; }

int craft_link_send(const void *data, int len) {
    if (s_state != CRAFT_LINK_CONNECTED || len <= 0) return 0;
    /* Model the device's small CDC FIFO: accept at most 256 bytes per
     * call so craft_net's pacing behaves the same on host as on the
     * cable (a raw socket would swallow the whole world transfer in one
     * frame and hide flow-control bugs). */
    if (len > 256) len = 256;
    ssize_t w = send(s_fd, data, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (w < 0) {
        if (errno == EPIPE || errno == ECONNRESET) {
            close_conn();
            s_state = CRAFT_LINK_SEARCHING;
        }
        return 0;
    }
    return (int)w;
}

int craft_link_recv(void *buf, int max) {
    if (s_state != CRAFT_LINK_CONNECTED || max <= 0) return 0;
    ssize_t r = recv(s_fd, buf, (size_t)max, MSG_DONTWAIT);
    if (r < 0) return 0;
    if (r == 0) { close_conn(); s_state = CRAFT_LINK_SEARCHING; return 0; }
    return (int)r;
}
