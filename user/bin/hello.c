/* hello — the simplest user program: arguments, pid, privilege level. */
#include "libc.h"

int main(int argc, char **argv) {
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));     /* low 2 bits = current privilege level */
    printf("Hello from user space!\n");
    printf("  pid %d, running at privilege level (ring) %d\n", getpid(), cs & 3);
    printf("  my code is at %p, my stack is near %p\n", (uint32_t)main, (uint32_t)&cs);
    printf("  argc = %d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 0;
}
