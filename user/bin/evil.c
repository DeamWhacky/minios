/* evil — a misbehaving program. Each attack is stopped by the CPU or the
 * kernel, the process is terminated (or gets an error), and MiniOS keeps
 * running. */
#include "libc.h"

static void usage(void) {
    printf("usage: evil <attack>\n"
           "  cli      disable interrupts (privileged instruction)\n"
           "  hlt      halt the CPU (privileged instruction)\n"
           "  io       write to a hardware I/O port\n"
           "  kread    read kernel memory directly\n"
           "  kwrite   overwrite kernel code directly\n"
           "  vga      scribble on the screen buffer directly\n"
           "  syscall  ask the kernel to read() into kernel memory\n"
           "  null     dereference a NULL pointer\n"
           "  loop     spin forever (stop it with Ctrl+C)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const char *a = argv[1];
    printf("evil: attempting '%s'...\n", a);

    if (!strcmp(a, "cli"))   __asm__ volatile("cli");
    else if (!strcmp(a, "hlt"))   __asm__ volatile("hlt");
    else if (!strcmp(a, "io"))    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    else if (!strcmp(a, "kread")) printf("kernel says: %x\n", *(volatile uint32_t *)0x00100000);
    else if (!strcmp(a, "kwrite")) *(volatile uint32_t *)0x00100000 = 0xDEADBEEF;
    else if (!strcmp(a, "vga"))   *(volatile uint16_t *)0xB8000 = 0x4F21;
    else if (!strcmp(a, "null"))  printf("%d\n", *(volatile int *)0);
    else if (!strcmp(a, "syscall")) {
        int r = read(STDIN, (void *)0x00100000, 64);
        printf("evil: kernel answered %d (%s). Nice try.\n", r, strerror(r));
        r = open((const char *)0x00100000, O_RDONLY);
        printf("evil: open(kernel pointer) -> %d (%s)\n", r, strerror(r));
        return 0;
    }
    else if (!strcmp(a, "loop")) { for (;;) ; }
    else { usage(); return 1; }

    printf("evil: ...it worked?! (this line should never appear)\n");
    return 0;
}
