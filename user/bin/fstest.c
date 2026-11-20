/* fstest — exercises the file system through system calls only:
 * mkdir, open/create, write, lseek, read, stat, readdir, unlink. */
#include "libc.h"

static void check(const char *what, int r) {
    printf("  %-34s -> %d%s%s\n", what, r, r < 0 ? "  " : "", r < 0 ? strerror(r) : "");
}

int main(void) {
    char buf[128];
    printf("fstest (pid %d): file system via system calls\n", getpid());

    check("mkdir(\"/tmp/fstest\")", mkdir("/tmp/fstest"));
    check("chdir(\"/tmp/fstest\")", chdir("/tmp/fstest"));
    getcwd(buf, sizeof buf);
    printf("  getcwd()                           -> %s\n", buf);

    int fd = open("notes.txt", O_WRONLY | O_CREAT | O_TRUNC);
    check("open(\"notes.txt\", O_CREAT)", fd);
    for (int i = 1; i <= 3; i++) {
        int n = snprintf(buf, sizeof buf, "line %d written by pid %d\n", i, getpid());
        write(fd, buf, n);
    }
    check("close(fd)", close(fd));

    fd = open("notes.txt", O_RDONLY);
    int n = read(fd, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    printf("  read back %d bytes:\n%s", n, buf);
    check("lseek(fd, 7, SEEK_SET)", lseek(fd, 7, SEEK_SET));
    n = read(fd, buf, 1); buf[1] = 0;
    printf("  byte at offset 7 is '%s'\n", buf);
    close(fd);

    stat_t st;
    stat("notes.txt", &st);
    printf("  stat: inode %u, %u bytes, %u block(s)\n", st.ino, st.size, st.blocks);

    check("open(\"/no/such/file\")", open("/no/such/file", O_RDONLY));
    check("unlink(\"/tmp\") (not empty)", unlink("/tmp"));

    printf("  contents of /:\n");
    fd = open("/", O_RDONLY);
    dirent_t de;
    while (readdir(fd, &de) == 1) printf("    %-12s inode %u\n", de.name, de.ino);
    close(fd);

    /* hard links: two names, one inode; the data survives until both are gone */
    check("link(\"notes.txt\", \"alias.txt\")", link("notes.txt", "alias.txt"));
    stat("alias.txt", &st);
    printf("  stat alias.txt: inode %u, %u links\n", st.ino, st.nlink);
    check("unlink(\"notes.txt\")", unlink("notes.txt"));
    fd = open("alias.txt", O_RDONLY);
    n = read(fd, buf, 6); buf[n > 0 ? n : 0] = 0;
    close(fd);
    printf("  alias.txt still reads '%s'\n", buf);
    check("unlink(\"alias.txt\")", unlink("alias.txt"));
    chdir("/");
    check("unlink(\"/tmp/fstest\")", unlink("/tmp/fstest"));
    printf("fstest done.\n");
    return 0;
}
