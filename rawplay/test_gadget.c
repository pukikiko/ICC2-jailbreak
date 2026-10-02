/*
 * test_gadget - the functionfs byte bridge, exercised without a udc. this includes
 * rawlink.c so the static bridge_client/pump_thread code runs against fifos standing in
 * for ep1/ep2 and a socketpair standing in for the stream client. the checks are the
 * real-car behaviours that cannot be reached on a laptop with no usb device controller:
 *
 *   * client bytes reach ep1 in whole maxpackets, with a sub-packet tail only when the
 *     sender pauses (the unit can wait for exactly those bytes, so holding them would
 *     deadlock);
 *   * unit bytes on ep2 reach the client unchanged;
 *   * a disable/re-enable generation change reopens the endpoints and keeps working;
 *   * the client going away ends the bridge instead of spinning.
 *
 *     cc -O1 -g -pthread -o out/test_gadget test_gadget.c -ldl && out/test_gadget
 */
#define main rawlink_main
#include "rawlink.c"
#undef main

#include <sys/stat.h>

static char dir[64];
static int failures;

static void expect(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static int read_wait(int fd, unsigned char *buf, size_t len, int ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    size_t off = 0;

    while (off < len && ms > 0) {
        int r = poll(&p, 1, ms < 50 ? ms : 50);
        ssize_t n;

        ms -= 50;
        if (r <= 0) {
            continue;
        }
        n = read(fd, buf + off, len - off);
        if (n <= 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
        off += (size_t)n;
    }
    return (int)off;
}

static void *bridge_thread(void *arg)
{
    struct gadget *g = arg;

    bridge_client(g, g->client_fd);
    return NULL;
}

static void mkfifo_quiet(const char *path)
{
    unlink(path);
    if (mkfifo(path, 0600) < 0) {
        fprintf(stderr, "mkfifo %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

int main(void)
{
    struct gadget g;
    pthread_t bt, pt;
    char ep1[PATH_MAX], ep2[PATH_MAX];
    int sv[2], r1, w2, i;
    unsigned char buf[1024];
    unsigned char pattern[700];

    snprintf(dir, sizeof dir, "/tmp/rawlink-gadget-XXXXXX");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    pjoin(ep1, sizeof ep1, dir, "/ep1");
    pjoin(ep2, sizeof ep2, dir, "/ep2");

    memset(&g, 0, sizeof g);
    pthread_mutex_init(&g.cli_mu, NULL);
    g.ep0 = -1;
    g.server_fd = -1;
    g.client_fd = -1;
    g.up_r = g.up_w = -1;
    g.vid_r = g.vid_w = -1;
    s_copy(g.ffs, sizeof g.ffs, dir);

    mkfifo_quiet(ep1);
    mkfifo_quiet(ep2);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return 2;
    }
    set_nonblock(sv[0]);
    g.client_fd = sv[0];
    g.enabled = 1;
    g.gen = 1;

    pthread_create(&bt, NULL, bridge_thread, &g);
    pthread_create(&pt, NULL, pump_thread, &g);

    /* whole maxpackets first, the sub-packet tail only once the client pauses */
    r1 = open(ep1, O_RDONLY | O_NONBLOCK);
    for (i = 0; i < 700; i++) {
        pattern[i] = (unsigned char)i;
    }
    (void)!write(sv[1], pattern, 700);
    {
        int got = read_wait(r1, buf, 512, 2000);

        expect(got == 512 && memcmp(buf, pattern, 512) == 0, "700 bytes arrive as 512+188");
        got = read_wait(r1, buf, 188, 2000);
        expect(got == 188 && memcmp(buf, pattern + 512, 188) == 0, "the 188 byte tail follows");
    }

    /* unit -> client: the pump hands LI bytes through unchanged */
    w2 = open(ep2, O_WRONLY | O_NONBLOCK);
    {
        unsigned char touch[8] = { 'L', 'I', 1, 0x7b, 0, 0x2d, 0, 1 };
        ssize_t n = write(w2, touch, sizeof touch);

        expect(n == (ssize_t)sizeof touch, "ep2 write accepted");
        memset(buf, 0, sizeof buf);
        expect(read_wait(sv[1], buf, sizeof touch, 2000) == (int)sizeof touch
               && memcmp(buf, touch, sizeof touch) == 0,
               "unit touch reaches the client unchanged");
    }

    /* disable and re-enable: the generation change must reopen the endpoints */
    g.enabled = 0;
    msleep(100);
    mkfifo_quiet(ep1);
    g.gen = 2;
    g.enabled = 1;
    msleep(100);
    close(r1);
    r1 = open(ep1, O_RDONLY | O_NONBLOCK);
    (void)!write(sv[1], pattern, 600);
    {
        int got = read_wait(r1, buf, 512, 2000);

        expect(got == 512, "bridge resumes after a disable/enable generation change");
        expect(read_wait(r1, buf, 88, 2000) == 88, "and keeps the framing");
    }

    /* the client going away ends the bridge */
    close(sv[1]);
    {
        int joined = 0;
        void *ret;

        for (i = 0; i < 40; i++) {
            if (pthread_tryjoin_np(bt, &ret) == 0) {
                joined = 1;
                break;
            }
            msleep(50);
        }
        expect(joined, "client close ends the bridge thread");
        if (!joined) {
            g.stop = 1;
            g.enabled = 0;
            pthread_join(bt, NULL);
        }
    }

    g.stop = 1;
    g.enabled = 0;
    pthread_join(pt, NULL);
    close(sv[0]);
    if (r1 >= 0) {
        close(r1);
    }
    close(w2);
    unlink(ep1);
    unlink(ep2);
    rmdir(dir);

    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
