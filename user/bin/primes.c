/* primes — CPU-bound user program. Run a few in the background
 * (primes 30000 & primes 30000 &) and watch them share the CPU with ps. */
#include "libc.h"

int main(int argc, char **argv) {
    uint32_t limit = argc > 1 ? (uint32_t)atoi(argv[1]) : 20000;
    uint32_t start = ticks(), count = 0, last = 0;
    for (uint32_t n = 2; n <= limit; n++) {
        bool prime = true;
        for (uint32_t d = 2; d * d <= n; d++) if (n % d == 0) { prime = false; break; }
        if (prime) { count++; last = n; }
    }
    printf("[pid %d] %u primes up to %u (largest %u) in %u ms\n",
           getpid(), count, limit, last, (ticks() - start) * 10);
    return 0;
}
