/* spawner — a user process that creates other user processes with the
 * spawn() system call and waits for each to finish (like fork+exec+wait). */
#include "libc.h"

int main(void) {
    printf("spawner (pid %d): starting 3 children\n", getpid());
    int pids[3];
    for (int i = 0; i < 3; i++) {
        char rounds[4];
        snprintf(rounds, sizeof rounds, "%d", 2 + i);
        char *argv[] = { "isolation", rounds, NULL };
        pids[i] = spawn("/bin/isolation", argv);
        printf("spawner: spawn -> pid %d\n", pids[i]);
    }
    for (int i = 0; i < 3; i++) {
        int code = wait(pids[i]);
        printf("spawner: child %d finished with code %d\n", pids[i], code);
    }
    return 0;
}
