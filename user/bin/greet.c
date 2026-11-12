/* greet — reads from the keyboard through the read() system call on fd 0. */
#include "libc.h"

int main(void) {
    char name[64];
    printf("What's your name? ");
    getline(name, sizeof name);
    if (!name[0]) strcpy(name, "stranger");
    printf("Nice to meet you, %s! I am process %d.\n", name, getpid());

    printf("Shall I save that to /home/guests.txt? (y/n) ");
    char ans[8];
    getline(ans, sizeof ans);
    if (ans[0] == 'y' || ans[0] == 'Y') {
        int fd = open("/home/guests.txt", O_WRONLY | O_CREAT | O_APPEND);
        if (fd < 0) { printf("open failed: %s\n", strerror(fd)); return 1; }
        dprintf(fd, "%s\n", name);
        close(fd);
        printf("Saved. Try: cat /home/guests.txt\n");
    }
    return 0;
}
