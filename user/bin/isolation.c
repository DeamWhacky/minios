/* isolation — run two copies at once:  isolation & isolation
 * Both print the SAME address for 'secret' but different values, because
 * each process has its own page tables mapping that address to its own
 * physical page. */
#include "libc.h"

static int secret;                 /* lives at the same virtual address in every copy */

int main(int argc, char **argv) {
    int pid = getpid();
    int rounds = argc > 1 ? atoi(argv[1]) : 5;
    secret = pid * 1000;
    for (int i = 0; i < rounds; i++) {
        secret++;
        printf("[pid %d] &secret = %p   secret = %d\n", pid, (uint32_t)&secret, secret);
        sleep_ms(300);
    }
    return 0;
}
