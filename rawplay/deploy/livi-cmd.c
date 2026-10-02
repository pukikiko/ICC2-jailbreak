/*
 * livi-cmd - send one line to LIVI's helper control socket and print the JSON reply.
 * The socket is the helper's /tmp/cp-bt.sock, which lives inside the glibc chroot and is
 * therefore the host's /opt/livi/rootfs/tmp/cp-bt.sock. It is the same channel the app
 * itself uses for its runtime wireless toggles:
 *
 *     set-aa 1   bring LIVI's wireless Android Auto up (AP + Bluetooth advertising)
 *     set-aa 0   tear it down again, handing wlan0 back to NetworkManager
 *
 * Wireless Android Auto is parked on in LIVI's config (livi-config.json,
 * wirelessAaEnabled) and left alone at runtime: livi-link-monitor does not touch it, so
 * this is the administrative tool for bringing it down by hand (Wi-Fi/ssh back) or up
 * again.
 *
 *     livi-cmd /opt/livi/rootfs/tmp/cp-bt.sock "set-aa 0"
 *
 * One RPC per connection, matching the helper's control loop. Exit 0 when the helper
 * replied {"ok": true}, 1 on connect/write/read failure or an ok:false reply, 2 on usage
 * errors.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct sockaddr_un addr;
    struct timeval tv = { 2, 0 };
    char line[512], buf[512];
    size_t len;
    ssize_t n;
    int fd;

    if (argc != 3) {
        fprintf(stderr, "usage: %s SOCKET 'COMMAND [ARG]'\n", argv[0]);
        return 2;
    }
    if (strlen(argv[1]) >= sizeof addr.sun_path) {
        fprintf(stderr, "%s: socket path too long\n", argv[0]);
        return 2;
    }
    len = strlen(argv[2]);
    if (len + 2 > sizeof line) {
        fprintf(stderr, "%s: command too long\n", argv[0]);
        return 2;
    }
    memcpy(line, argv[2], len);
    line[len] = '\n';

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, argv[1], strlen(argv[1]) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror(argv[1]);
        close(fd);
        return 1;
    }
    if (write(fd, line, len + 1) != (ssize_t)(len + 1)) {
        perror("write");
        close(fd);
        return 1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) {
        fprintf(stderr, "no reply from %s\n", argv[1]);
        return 1;
    }
    buf[n] = 0;
    fputs(buf, stdout);
    return strstr(buf, "\"ok\": true") ? 0 : 1;
}
