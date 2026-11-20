/* shell.c — the interactive command shell.
 *
 * Runs as a kernel task. Built-in commands are handled here; anything else
 * is looked up in /bin and started as a separate USER-MODE process, which
 * the shell waits for (or not, with a trailing '&'). */
#include "demos.h"
#include "task.h"
#include "memory.h"
#include "console.h"
#include "drivers.h"
#include "fs.h"
#include "process.h"

#define LINE_MAX 160
#define MAX_ARGS 16

typedef struct {
    const char *name;
    const char *usage;
    const char *help;
    void (*fn)(int argc, char **argv);
} command_t;

static void cmd_help(int, char **);

static void err(const char *what, int code) {
    console_set_color(C_LRED, C_BLACK);
    kprintf("%s: %s\n", what, strerror(code));
    console_set_color(C_LGREY, C_BLACK);
}

/* ======================= processes & system ======================= */
static void cmd_clear(int argc, char **argv) { (void)argc; (void)argv; console_clear(); }

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t now = timer_ticks();
    task_t *tab = task_table();
    kprintf("PID  NAME            MODE    STATE     PRIO  CPU(ms)  CPU%%  SWITCH  MEM(KiB)\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tab[i];
        if (t->state == T_UNUSED) continue;
        uint32_t life = now - t->created_tick;
        uint32_t pct = life ? t->cpu_ticks * 100 / life : 0;
        uint32_t mem = TASK_STACK_SIZE / 1024 + paging_count_user_pages(t->page_dir) * 4;
        kprintf("%3d  %-15s %-7s %-9s %4d %8u %4u%% %7u %9u\n", t->pid, t->name,
                t->is_user ? "user" : "kernel", task_state_name(t->state),
                t->priority, t->cpu_ticks * 10, pct, t->switches, mem);
    }
}

static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t tf = pmm_total_count(), ff = pmm_free_count();
    kprintf("Physical: %u KiB total, %u KiB used, %u KiB free (%u frames of 4 KiB)\n",
            tf * 4, (tf - ff) * 4, ff * 4, tf);
    uint32_t total, used, blocks, free_blocks;
    heap_stats(&total, &used, &blocks, &free_blocks);
    kprintf("Heap:     %u KiB mapped at %p, %u B in use, %u blocks (%u free)\n",
            total / 1024, HEAP_START, used, blocks, free_blocks);
    kprintf("Kernel:   0-%u MiB identity mapped, supervisor-only; page 0 unmapped\n", IDENTITY_MAP_MB);
    kprintf("User:     %p-%p per process (code+data from %p, 16 KiB stack below %p)\n",
            USER_BASE, USER_TOP, USER_BASE, USER_STACK_TOP);
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t t = timer_ticks();
    kprintf("Up %u.%02u s (%u ticks), %u context switches\n",
            t / TIMER_HZ, t % TIMER_HZ, t, sched_context_switches());
}

static void cmd_sched(int argc, char **argv) {
    if (argc >= 2) {
        if      (!strcmp(argv[1], "rr"))   sched_set_policy(SCHED_RR);
        else if (!strcmp(argv[1], "prio")) sched_set_policy(SCHED_PRIORITY);
        else if (!strcmp(argv[1], "fcfs")) sched_set_policy(SCHED_FCFS);
        else { kprintf("unknown policy '%s' (use rr, prio, fcfs)\n", argv[1]); return; }
    }
    kprintf("Scheduler: %s, quantum %u ticks (%u ms), aging %s\n",
            sched_policy_name(sched_get_policy()), sched_get_quantum(),
            sched_get_quantum() * 1000 / TIMER_HZ, sched_get_aging() ? "on" : "off");
}

static void cmd_quantum(int argc, char **argv) {
    if (argc < 2 || atoi(argv[1]) <= 0) { kprintf("usage: quantum <ticks>\n"); return; }
    sched_set_quantum(atoi(argv[1]));
    kprintf("Time quantum = %u ticks (%u ms)\n", sched_get_quantum(), sched_get_quantum() * 1000 / TIMER_HZ);
}

static void cmd_aging(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: aging on|off\n"); return; }
    sched_set_aging(!strcmp(argv[1], "on"));
    kprintf("Aging %s\n", sched_get_aging() ? "enabled" : "disabled");
}

static void cmd_spawn(int argc, char **argv) {
    int prio = argc >= 2 ? atoi(argv[1]) : PRIO_DEFAULT;
    int pid = spawn_spinner(prio);
    if (pid < 0) kprintf("spawn failed (task table full?)\n");
    else kprintf("Started CPU-bound kernel thread 'spinner' pid %d, priority %d\n", pid, prio);
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: kill <pid>\n"); return; }
    int pid = atoi(argv[1]);
    if (pid == task_current()->pid) { kprintf("refusing to kill the shell\n"); return; }
    if (task_kill(pid) == 0) kprintf("Killed %d\n", pid);
    else kprintf("No such task %d (or it can't be killed)\n", pid);
}

static void cmd_nice(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: nice <pid> <0-9>\n"); return; }
    if (task_set_priority(atoi(argv[1]), atoi(argv[2])))
        kprintf("Priority of %d set to %d\n", atoi(argv[1]), atoi(argv[2]));
    else kprintf("failed\n");
}

static void cmd_demo(int argc, char **argv) {
    if (argc < 2) { kprintf("demos: sched, prodcons, philo, race, syscall, heap\n"); return; }
    if      (!strcmp(argv[1], "sched"))    demo_sched();
    else if (!strcmp(argv[1], "prodcons")) demo_prodcons();
    else if (!strcmp(argv[1], "philo"))    demo_philosophers();
    else if (!strcmp(argv[1], "race"))     demo_race();
    else if (!strcmp(argv[1], "syscall"))  demo_syscalls();
    else if (!strcmp(argv[1], "heap"))     demo_heap();
    else kprintf("unknown demo '%s'\n", argv[1]);
}

static void cmd_crash(int argc, char **argv) {
    int kind = 0;
    if (argc >= 2) {
        if (!strcmp(argv[1], "unmapped")) kind = 1;
        else if (!strcmp(argv[1], "div0")) kind = 2;
    }
    demo_crash(kind);
}

static void cmd_reboot(int argc, char **argv)   { (void)argc; (void)argv; system_reboot(); }
static void cmd_shutdown(int argc, char **argv) { (void)argc; (void)argv; system_shutdown(); }

/* ======================= files ======================= */
static void print_entry(const char *name, uint32_t ino, bool long_fmt) {
    stat_t st;
    if (fs_stat_ino(ino, &st) < 0) return;
    if (long_fmt) kprintf("%c %4u %6u  ", st.type == FT_DIR ? 'd' : '-', st.ino, st.size);
    if (st.type == FT_DIR) console_set_color(C_LBLUE, C_BLACK);
    kprintf("%s%s", name, st.type == FT_DIR ? "/" : "");
    console_set_color(C_LGREY, C_BLACK);
    kprintf(long_fmt ? "\n" : "  ");
}

static void cmd_ls(int argc, char **argv) {
    bool long_fmt = false;
    const char *path = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) long_fmt = true; else path = argv[i];
    }
    int ino = fs_lookup(path);
    if (ino < 0) { err(path, ino); return; }
    stat_t st;
    fs_stat_ino(ino, &st);
    if (st.type != FT_DIR) { print_entry(path, ino, long_fmt); if (!long_fmt) kprintf("\n"); return; }
    if (long_fmt) kprintf("T  INO   SIZE  NAME\n");
    uint32_t off = 0;
    dirent_t de;
    while (fs_readdir(ino, &off, &de) == 1) {
        if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
        print_entry(de.name, de.ino, long_fmt);
    }
    if (!long_fmt) kprintf("\n");
}

static void tree(uint32_t ino, int depth) {
    uint32_t off = 0;
    dirent_t de;
    while (fs_readdir(ino, &off, &de) == 1) {
        if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
        for (int i = 0; i < depth; i++) kprintf("|   ");
        kprintf("|-- ");
        print_entry(de.name, de.ino, false);
        kprintf("\n");
        stat_t st;
        fs_stat_ino(de.ino, &st);
        if (st.type == FT_DIR && depth < 8) tree(de.ino, depth + 1);
    }
}

static void cmd_tree(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    int ino = fs_lookup(path);
    if (ino < 0) { err(path, ino); return; }
    kprintf("%s\n", path);
    tree(ino, 0);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: cat <file>...\n"); return; }
    char buf[256];
    for (int i = 1; i < argc; i++) {
        int fd = fd_open(argv[i], O_RDONLY);           /* the shell uses fds too */
        if (fd < 0) { err(argv[i], fd); continue; }
        int n;
        while ((n = fd_read(fd, buf, sizeof buf)) > 0) console_write_n(buf, n);
        if (n < 0) err(argv[i], n);
        fd_close(fd);
    }
}

static void cmd_touch(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int r = fs_lookup(argv[i]);
        if (r == -E_NOENT) r = fs_create(argv[i], FT_FILE);
        if (r < 0) err(argv[i], r);
    }
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: mkdir <dir>...\n"); return; }
    for (int i = 1; i < argc; i++) {
        int r = fs_create(argv[i], FT_DIR);
        if (r < 0) err(argv[i], r);
    }
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: rm <file|empty dir>...\n"); return; }
    for (int i = 1; i < argc; i++) {
        int r = fs_unlink(argv[i]);
        if (r < 0) err(argv[i], r);
    }
}

static void cmd_cd(int argc, char **argv) {
    int r = fs_chdir(argc > 1 ? argv[1] : "/");
    if (r < 0) err(argc > 1 ? argv[1] : "/", r);
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[PATH_MAX];
    if (fs_getcwd(buf, sizeof buf) >= 0) kprintf("%s\n", buf);
}

static void cmd_stat(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: stat <path>\n"); return; }
    stat_t st;
    int r = fs_stat(argv[1], &st);
    if (r < 0) { err(argv[1], r); return; }
    kprintf("  File: %s\n  Type: %s   Inode: %u\n  Size: %u bytes in %u blocks of %u bytes\n",
            argv[1], st.type == FT_DIR ? "directory" : "regular file", st.ino,
            st.size, st.blocks, BLOCK_SIZE);
    kprintf("  Created: %u.%02u s after boot   Modified: %u.%02u s after boot\n",
            st.ctime / 100, st.ctime % 100, st.mtime / 100, st.mtime % 100);
}

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t ub, tb, ui, ti;
    fs_usage(&ub, &tb, &ui, &ti);
    kprintf("MiniFS (RAM disk)\n  Blocks: %u / %u used (%u KiB / %u KiB), %u bytes each\n"
            "  Inodes: %u / %u used\n  Max file size: %u KiB (12 direct + 128 indirect blocks)\n",
            ub, tb, ub * BLOCK_SIZE / 1024, tb * BLOCK_SIZE / 1024, BLOCK_SIZE, ui, ti,
            MAX_FILE_SIZE / 1024);
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: cp <src> <dst>\n"); return; }
    stat_t st;
    int r = fs_stat(argv[1], &st);
    if (r < 0) { err(argv[1], r); return; }
    if (st.type == FT_DIR) { err(argv[1], -E_ISDIR); return; }

    char dst[PATH_MAX];
    strncpy(dst, argv[2], PATH_MAX - 1); dst[PATH_MAX - 1] = 0;
    stat_t dst_st;
    if (fs_stat(dst, &dst_st) == 0 && dst_st.type == FT_DIR) {   /* cp file dir/ */
        const char *base = argv[1];
        for (const char *p = argv[1]; *p; p++) if (*p == '/') base = p + 1;
        uint32_t n = strlen(dst);
        if (n + 1 + strlen(base) >= PATH_MAX) { err(dst, -E_NAMETOOLONG); return; }
        dst[n] = '/'; strncpy(dst + n + 1, base, PATH_MAX - n - 2);
    }
    char *buf = kmalloc(st.size + 1);
    if (!buf) { err("cp", -E_NOMEM); return; }
    fs_read(st.ino, 0, buf, st.size);
    r = fs_write_file(dst, buf, st.size);
    kfree(buf);
    if (r < 0) err(dst, r);
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: mv <src> <dst>\n"); return; }
    int r = fs_rename(argv[1], argv[2]);
    if (r < 0) err(argv[1], r);
}

/* echo text          -> print
 * echo text > file   -> overwrite file
 * echo text >> file  -> append to file */
static void cmd_echo(int argc, char **argv) {
    int redirect = 0, end = argc;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], ">"))  { redirect = 1; end = i; break; }
        if (!strcmp(argv[i], ">>")) { redirect = 2; end = i; break; }
    }
    char line[LINE_MAX];
    uint32_t n = 0;
    for (int i = 1; i < end; i++) {
        for (const char *s = argv[i]; *s && n < LINE_MAX - 2; s++) line[n++] = *s;
        if (i + 1 < end && n < LINE_MAX - 2) line[n++] = ' ';
    }
    line[n++] = '\n';
    if (!redirect) { console_write_n(line, n); return; }
    if (end + 1 >= argc) { kprintf("echo: missing file name\n"); return; }

    int flags = O_WRONLY | O_CREAT | (redirect == 1 ? O_TRUNC : O_APPEND);
    int fd = fd_open(argv[end + 1], flags);
    if (fd < 0) { err(argv[end + 1], fd); return; }
    int r = fd_write(fd, line, n);
    if (r < 0) err(argv[end + 1], r);
    fd_close(fd);
}

/* ======================= running programs ======================= */
static void cmd_programs(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("User programs in /bin (run by typing the name, add '&' to run in background):\n");
    uint32_t off = 0;
    dirent_t de;
    int ino = fs_lookup("/bin");
    if (ino < 0) return;
    while (fs_readdir(ino, &off, &de) == 1)
        if (strcmp(de.name, ".") && strcmp(de.name, "..")) kprintf("  %s", de.name);
    kprintf("\n");
}

static bool run_program(int argc, char **argv) {
    bool background = false;
    if (argc > 1 && !strcmp(argv[argc - 1], "&")) { background = true; argc--; }

    char path[PATH_MAX];
    if (strchr(argv[0], '/')) strncpy(path, argv[0], PATH_MAX - 1);
    else {
        strncpy(path, "/bin/", PATH_MAX);
        strncpy(path + 5, argv[0], PATH_MAX - 6);
    }
    path[PATH_MAX - 1] = 0;

    stat_t st;
    if (fs_stat(path, &st) < 0) return false;            /* not a program either */

    int pid = process_spawn(path, argc, argv, task_current()->priority > 5 ? 5 : task_current()->priority);
    if (pid < 0) { err(argv[0], pid); return true; }

    if (background) {
        task_detach(pid);
        kprintf("[%d] %s running in background\n", pid, argv[0]);
        return true;
    }

    input_set_foreground(pid);                            /* Ctrl+C now kills it */
    int code = task_wait(pid);
    input_set_foreground(0);
    if (code == -9)       kprintf("^C  [%d] killed\n", pid);
    else if (code == -11) kprintf("[%d] terminated by the kernel (exception)\n", pid);
    else if (code != 0)   kprintf("[%d] exited with code %d\n", pid, code);
    return true;
}

static const command_t commands[] = {
    { "help",     "",                    "show this list",                          cmd_help },
    { "programs", "",                    "list user programs in /bin",              cmd_programs },
    { "ls",       "[-l] [path]",         "list a directory",                        cmd_ls },
    { "tree",     "[path]",              "show a directory tree",                   cmd_tree },
    { "cd",       "[dir]",               "change directory",                        cmd_cd },
    { "pwd",      "",                    "print working directory",                 cmd_pwd },
    { "cat",      "<file>...",           "print files",                             cmd_cat },
    { "echo",     "<text> [>|>> file]",  "print, or write/append to a file",        cmd_echo },
    { "touch",    "<file>...",           "create empty files",                      cmd_touch },
    { "mkdir",    "<dir>...",            "create directories",                      cmd_mkdir },
    { "rm",       "<path>...",           "remove files / empty directories",        cmd_rm },
    { "cp",       "<src> <dst>",         "copy a file",                             cmd_cp },
    { "mv",       "<src> <dst>",         "move / rename",                           cmd_mv },
    { "stat",     "<path>",              "inode details",                           cmd_stat },
    { "df",       "",                    "file system usage",                       cmd_df },
    { "ps",       "",                    "list tasks and processes",                cmd_ps },
    { "kill",     "<pid>",               "terminate a task",                        cmd_kill },
    { "nice",     "<pid> <prio>",        "change a task's priority (0-9)",          cmd_nice },
    { "spawn",    "[prio]",              "start a background CPU-bound thread",     cmd_spawn },
    { "sched",    "[rr|prio|fcfs]",      "show / set scheduling policy",            cmd_sched },
    { "quantum",  "<ticks>",             "set time slice (1 tick = 10 ms)",         cmd_quantum },
    { "aging",    "on|off",              "priority aging (anti-starvation)",        cmd_aging },
    { "mem",      "",                    "memory usage and layout",                 cmd_mem },
    { "uptime",   "",                    "time since boot",                         cmd_uptime },
    { "demo",     "<name>",              "sched|prodcons|philo|race|syscall|heap",  cmd_demo },
    { "crash",    "[null|unmapped|div0]","crash a kernel thread (fault isolation)", cmd_crash },
    { "clear",    "",                    "clear the screen",                        cmd_clear },
    { "reboot",   "",                    "reboot the machine",                      cmd_reboot },
    { "shutdown", "",                    "power off (exits QEMU)",                  cmd_shutdown },
};
#define NCOMMANDS (sizeof commands / sizeof commands[0])

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    for (size_t i = 0; i < NCOMMANDS; i++) {
        char left[40]; int k = 0;
        for (const char *s = commands[i].name; *s; s++) left[k++] = *s;
        if (*commands[i].usage) { left[k++] = ' '; for (const char *s = commands[i].usage; *s && k < 38; s++) left[k++] = *s; }
        left[k] = 0;
        kprintf("  %-30s %s\n", left, commands[i].help);
    }
    kprintf("Anything else runs a program from /bin (type 'programs'). Ctrl+C stops it.\n");
}

/* ======================= main loop ======================= */
static int read_line(char *buf, int max) {
    int n = 0;
    for (;;) {
        char c = input_getchar();              /* blocks: shell uses no CPU while idle */
        if (c == '\n') { console_putc('\n'); buf[n] = 0; return n; }
        if (c == '\b') { if (n > 0) { n--; console_backspace(); } continue; }
        if (c == 3)    { kprintf("^C\n"); buf[0] = 0; return 0; }
        if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = c; console_putc(c); }
    }
}

static int tokenize(char *line, char **argv) {
    int argc = 0;
    while (*line && argc < MAX_ARGS) {
        while (*line == ' ') *line++ = 0;
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
    }
    return argc;
}

void shell_main(void *arg) {
    (void)arg;
    char line[LINE_MAX];
    char *argv[MAX_ARGS];
    char cwd[PATH_MAX];
    cmd_cat(2, (char *[]){ "cat", "/etc/motd" });
    for (;;) {
        fs_getcwd(cwd, sizeof cwd);
        console_set_color(C_LGREEN, C_BLACK);
        kprintf("minios");
        console_set_color(C_LBLUE, C_BLACK);
        kprintf(":%s", cwd);
        console_set_color(C_LGREY, C_BLACK);
        kprintf("> ");
        read_line(line, sizeof line);
        int argc = tokenize(line, argv);
        if (!argc) continue;
        size_t i;
        for (i = 0; i < NCOMMANDS; i++)
            if (!strcmp(argv[0], commands[i].name)) { commands[i].fn(argc, argv); break; }
        if (i == NCOMMANDS && !run_program(argc, argv))
            kprintf("unknown command '%s', try 'help'\n", argv[0]);
    }
}
