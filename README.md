# MiniOS — a small x86 operating system

A 32-bit x86 operating system (~4,200 lines of C and assembly) that boots in QEMU and shows
the core ideas of an OS working for real: interrupts, preemptive multitasking with three
scheduling policies, paging with a separate address space per process, user mode (ring 3)
with protection, an inode file system with directories and file descriptors, system calls,
blocking synchronization, and an interactive shell that runs user programs from `/bin`.

MiniOS is its own operating system. It is not based on Linux or Windows; it runs inside QEMU,
an emulator that pretends to be a PC. Linux (or WSL on Windows) is only used to build it.

## Quick start

**Linux / WSL (Ubuntu/Debian):**
```bash
sudo apt install build-essential gcc-multilib nasm qemu-system-x86
make run              # QEMU window + serial log in your terminal
make run-nographic    # everything in the terminal (quit QEMU: Ctrl+A then X)
```

**macOS:** install a cross compiler, then build with it:
```bash
brew install i686-elf-gcc i686-elf-binutils nasm qemu
make CC=i686-elf-gcc LD=i686-elf-ld run
```

QEMU loads the kernel directly with `-kernel` (Multiboot), so there is no bootloader or disk
image to set up. `make debug` starts QEMU paused for gdb:
`gdb build/minios.elf -ex "target remote :1234"`.

## A guided tour

```
minios:/> help
minios:/> ls -l /                 # the file system
minios:/> tree /
minios:/> cat /home/readme.txt
minios:/> echo first note > /home/notes.txt
minios:/> echo second note >> /home/notes.txt
minios:/> cat /home/notes.txt
minios:/> wc /home/notes.txt      # a USER program reading a file via syscalls
minios:/> stat /home/notes.txt    # inode number, size, blocks
minios:/> df                      # disk usage

minios:/> programs                # user programs installed in /bin
minios:/> hello a b c             # runs in ring 3, prints its pid and privilege level
minios:/> isolation &             # two copies, same address, different values:
minios:/> isolation               #   each process has its own address space
minios:/> evil kwrite             # tries to overwrite the kernel -> killed by a page fault
minios:/> evil cli                # privileged instruction -> killed by a protection fault
minios:/> evil syscall            # passes a kernel pointer to read() -> refused
minios:/> evil loop               # infinite loop -> press Ctrl+C
minios:/> fstest                  # the file system API from user space
minios:/> greet                   # reads keyboard input through read(0, ...)
minios:/> spawner                 # a user process that spawns and waits for others
minios:/> primes 40000 & primes 40000 &
minios:/> ps                      # user and kernel tasks, CPU time, memory

minios:/> demo sched              # 4 jobs under round robin: Gantt chart + timing table
minios:/> sched prio
minios:/> demo sched              # same jobs, priority scheduling
minios:/> sched fcfs
minios:/> demo sched              # first-come-first-served
minios:/> demo race               # race condition without a lock, fixed with a mutex
minios:/> demo prodcons           # bounded buffer with semaphores
minios:/> demo philo              # dining philosophers, no deadlock
minios:/> demo syscall            # a kernel thread using only int 0x80
```

### Starvation and aging (priority policy)
```
minios:/> sched prio
minios:/> aging off
minios:/> spawn 9             # kernel thread with higher priority than the shell (7)...
```
The shell now **starves**: your keystrokes are queued but it never gets the CPU. Reboot, then
repeat with `aging on`: the shell's effective priority rises the longer it waits, and it
becomes responsive again after a few seconds. (Under `fcfs` a spinner likewise never gives up
the CPU, because FCFS is non-preemptive.)

## Shell commands

| Files | |
|---|---|
| `ls [-l] [path]`, `tree [path]` | list directories |
| `cd [dir]`, `pwd` | working directory (the prompt shows it) |
| `cat <file>...` | print files |
| `echo text`, `echo text > f`, `echo text >> f` | print, overwrite, append |
| `touch`, `mkdir`, `rm`, `cp`, `mv` | create, delete, copy, move/rename |
| `stat <path>`, `df` | inode details, disk usage |

| Processes & scheduling | |
|---|---|
| `<program> [args] [&]` | run `/bin/<program>` in user mode (`&` = background); Ctrl+C stops it |
| `programs` | list `/bin` |
| `ps` | PID, mode (user/kernel), state, priority, CPU, switches, memory |
| `kill <pid>`, `nice <pid> <prio>` | terminate / re-prioritise |
| `spawn [prio]` | start a CPU-bound kernel thread |
| `sched [rr\|prio\|fcfs]`, `quantum <ticks>`, `aging on\|off` | scheduler settings |

| System | |
|---|---|
| `mem`, `uptime` | memory layout and usage; uptime and context switches |
| `demo <name>` | `sched`, `prodcons`, `philo`, `race`, `syscall`, `heap` |
| `crash [null\|unmapped\|div0]` | crash a kernel thread to show fault isolation |
| `clear`, `reboot`, `shutdown` | |

## User programs (`user/bin/`)

| Program | Shows |
|---|---|
| `hello [args]` | ring 3 privilege level, argc/argv passing, pid |
| `isolation [n]` | separate address spaces: same virtual address, different data |
| `evil <attack>` | protection: `cli`, `hlt`, `io`, `kread`, `kwrite`, `vga`, `null`, `syscall`, `loop` |
| `fstest` | mkdir, open/create, write, lseek, read, stat, readdir, unlink, chdir, getcwd |
| `wc [files]` | reading files (or stdin) through file descriptors |
| `greet` | keyboard input via `read(0, ...)`, appending to a file |
| `primes [n]` | CPU-bound work for scheduling experiments |
| `spawner` | a process creating child processes (`spawn`) and waiting for them (`wait`) |

To add your own: drop `user/bin/myprog.c` (with `#include "libc.h"` and a `main`) and run
`make`. The Makefile compiles it, embeds it in the kernel, and it appears as `/bin/myprog`.

## Architecture

```
boot/boot.asm         Multiboot header, stack, jump to kmain
boot/interrupts.asm   GDT/IDT/TSS loading, interrupt stubs, isr_common (context switch)
kernel/kernel.c       kmain: initialise everything, populate the file system, start the shell
kernel/cpu.c          GDT (kernel + user segments), TSS, IDT, PIC, exception handling
kernel/drivers.c      PIT timer (100 Hz), PS/2 keyboard, input queue, Ctrl+C, power off
kernel/console.c      VGA text mode + COM1 serial, kprintf
kernel/memory.c       frame allocator, page directories per process, kernel heap
kernel/task.c         tasks, wait queues, scheduler (RR / priority+aging / FCFS)
kernel/process.c      ELF loader, user address spaces, argv, user-pointer validation
kernel/syscall.c      int 0x80 system call table (18 calls)
kernel/fs.c           MiniFS: blocks, inodes, directories, path resolution
kernel/file.c         file descriptors and the open-file table
kernel/sync.c         counting semaphores and mutexes
kernel/shell.c        command shell
kernel/demos.c        scheduling / synchronization / fault experiments
include/abi.h         the kernel <-> user contract: syscall numbers, flags, structs, errors
user/crt0.asm         user program start-up (calls main, then exit)
user/libc.c           user C library: syscall wrappers, printf, strings
user/bin/*.c          user programs, installed into /bin at boot
```

### Boot sequence
QEMU (acting as a Multiboot loader) puts the CPU in 32-bit protected mode and jumps to
`_start`. `kmain` sets up, in order: GDT + TSS → IDT → PIC → physical memory → paging → heap →
tasking → system calls → timer → keyboard → file system. It formats the RAM disk, creates
`/bin /etc /home /tmp`, copies the embedded user programs into `/bin`, starts the shell, and
then becomes **task 0, the idle task**, which executes `hlt` until there is other work.

### Interrupts and the context switch
Every interrupt enters `isr_common`, which pushes all registers onto the current task's kernel
stack and calls `interrupt_dispatch(regs_t *r)`. The C handler returns the stack pointer to
resume. If the scheduler chose another task, that pointer is on another task's stack, so the
final `mov esp, eax; pop...; iret` resumes a different task: that instruction *is* the context
switch. On a switch the scheduler also loads the next task's page directory into CR3 and
stores its kernel stack in the TSS.

### Scheduling
The PIT fires 100 times a second. On each tick `scheduler_tick()` wakes sleepers, charges the
tick to the running task, ages waiting tasks and decides whether to preempt:

| Policy | Picks | Preempts when |
|---|---|---|
| Round robin | head of the FIFO ready queue | quantum expires |
| Priority | highest *effective* priority (priority + age/100 ticks); FIFO among equals | a higher-priority task is ready, or quantum expires and an equal one waits |
| FCFS | head of the FIFO ready queue | never: only when the task blocks, sleeps, yields or exits |

Task states: `READY → RUNNING → (SLEEPING | BLOCKED) → READY … → ZOMBIE`. A zombie's kernel
stack, address space and open files are released at the next switch.

### User mode and protection
x86 has privilege rings; the kernel runs in ring 0 and user programs in ring 3.

* **Segments:** the GDT has kernel code/data (ring 0) and user code/data (ring 3) segments.
  Code running from the ring-3 segment cannot execute privileged instructions (`cli`, `hlt`,
  `in`/`out`, loading CR3…); trying raises a General Protection Fault.
* **TSS:** when an interrupt or system call arrives in ring 3, the CPU switches to the kernel
  stack named in the TSS. The scheduler updates it on every task switch.
* **Entering user mode:** a new process gets a fake interrupt frame with user segments, the
  program's entry point and its user stack. The first `iret` "returns" into ring 3.
* **Memory protection:** kernel pages are mapped supervisor-only, so user code touching them
  page-faults. User pages have the U bit set.
* **System call checking:** every pointer a program passes to the kernel is checked against
  its page tables (`user_ok`, `copy_user_str`). Otherwise a program could ask the kernel, which
  can write anywhere, to `read()` into kernel memory.
* **Faults:** a faulting process is terminated and its memory freed; the rest of the system
  carries on. Ctrl+C kills the foreground program safely (never mid-system-call).

### Memory
* **Physical memory manager:** a bitmap with one bit per 4 KiB frame.
* **Address spaces:** every process has its own page directory.

  ```
  0x00000000-0x03FFFFFF  kernel, identity mapped        shared, supervisor-only
  0x10000000-0x13FFFFFF  kernel heap                    shared, supervisor-only
  0x40000000-...         program code, data, bss        private, user
  0x4FFFC000-0x4FFFFFFF  user stack (16 KiB)            private, user
  ```
  The kernel half points at the same page tables in every directory, so the kernel is always
  mapped. The user half is private: two programs can use address `0x40001234` and see
  different memory (try `isolation`). Page 0 is unmapped so NULL dereferences fault.
* **Heap (`kmalloc`/`kfree`):** first-fit free list that splits blocks and merges free
  neighbours. Magic numbers catch invalid and double frees.

### File system (MiniFS)
An inode file system on a 1 MiB RAM disk (2048 blocks of 512 bytes), structured like a
simplified ext2 or the xv6 file system:

* **Blocks:** allocated from a free-block bitmap.
* **Inodes:** 128 of them; each has a type, size, timestamps, 12 *direct* block pointers and
  one *indirect* block holding 128 more pointers. Maximum file size is 70 KiB.
* **Directories:** files whose data is an array of 32-byte entries `{name, inode}`. Every
  directory contains `.` and `..`, which makes `cd ..` and `pwd` work.
* **Path resolution:** walks the path component by component from `/` or the process's
  working directory.
* **File descriptors:** each process has a table of 16 descriptors pointing into a global
  open-file table (inode + offset + flags), as in UNIX. Descriptors 0, 1, 2 are the console.
  Files that are open cannot be deleted.
* **Concurrency:** one mutex serialises file system operations.

Because it is a RAM disk, the file system is rebuilt at every boot.

### System calls (`int 0x80`)
EAX = number, EBX/ECX/EDX = arguments, result in EAX (negative = error code):

`exit, yield, sleep, getpid, ticks, read, write, open, close, lseek, unlink, mkdir, readdir,
stat, chdir, getcwd, spawn, wait`

The system call gate is a *trap gate*, so long system calls can be preempted by the timer.

### Synchronization
On a single CPU, disabling interrupts makes a short region atomic, so every primitive builds
on `irq_save()/irq_restore()`. A semaphore that can't be decremented puts the caller on its
wait queue (state `BLOCKED`) and switches away; `sem_signal` moves one waiter back to the ready
queue. Mutexes are binary semaphores that record their owner. Keyboard input uses the same
mechanism, which is why the idle shell uses 0% CPU.

## Design simplifications
* Single CPU; interrupt disabling plus one file-system mutex are the only locks.
* The shell is a kernel thread; programs it starts run in user mode.
* `spawn` creates a process directly from a file (like `posix_spawn`); there is no `fork`.
* The file system lives in RAM; there is no disk driver, so changes are lost on reboot.
* User programs have a fixed-size stack and no heap (`malloc`).
* A task that holds a mutex is not killed straight away: the kill waits until it releases
  its last mutex, so locked data is never left half-updated. A kernel thread that *faults*
  while holding a mutex still leaves it locked.

