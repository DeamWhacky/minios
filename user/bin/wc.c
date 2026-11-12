/* wc — count lines, words and bytes, like the UNIX tool. */
#include "libc.h"

static int count(int fd, const char *name) {
    char buf[256];
    uint32_t lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    int n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        bytes += n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            bool space = c == ' ' || c == '\n' || c == '\t';
            if (!space && !in_word) words++;
            in_word = !space;
        }
        if (fd == STDIN && buf[n - 1] == '\n' && n == 1) break;   /* empty line ends stdin */
    }
    if (n < 0) { printf("wc: %s: %s\n", name, strerror(n)); return 1; }
    printf("%7u %7u %7u %s\n", lines, words, bytes, name);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("(reading stdin, finish with an empty line)\n");
        return count(STDIN, "");
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { printf("wc: %s: %s\n", argv[i], strerror(fd)); rc = 1; continue; }
        rc |= count(fd, argv[i]);
        close(fd);
    }
    return rc;
}
