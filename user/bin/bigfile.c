/* bigfile [KiB] — writes a file bigger than the direct + indirect blocks can
 * hold (so it needs the double-indirect block), reads it back and checks
 * every byte, then deletes it. Default 512 KiB. */
#include "libc.h"

static char buf[1024];

int main(int argc, char **argv) {
    int kib = argc > 1 ? atoi(argv[1]) : 512;
    if (kib <= 0) kib = 512;
    const char *path = "/tmp/big.bin";
    printf("bigfile: writing %d KiB to %s\n", kib, path);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { printf("open: %s\n", strerror(fd)); return 1; }
    int written = 0;
    for (int k = 0; k < kib; k++) {
        for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (char)((k * 7 + i) & 0xFF);
        int n = write(fd, buf, sizeof buf);
        if (n < 0) { printf("write failed after %d KiB: %s\n", k, strerror(n)); break; }
        written += n;
        if (n < (int)sizeof buf) { printf("disk full after %d bytes\n", written); break; }
    }
    close(fd);

    stat_t st;
    stat(path, &st);
    printf("  size %u bytes, %u blocks of 512 bytes (data + pointer blocks)\n", st.size, st.blocks);

    fd = open(path, O_RDONLY);
    int bad = 0, total = 0;
    for (int k = 0; total < written; k++) {
        int n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (int i = 0; i < n; i++)
            if (buf[i] != (char)((k * 7 + i) & 0xFF)) bad++;
        total += n;
    }
    close(fd);
    printf("  read back %d bytes, %d wrong\n", total, bad);
    printf("  unlink -> %d\n", unlink(path));
    return bad ? 1 : 0;
}
