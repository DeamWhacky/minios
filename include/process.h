#ifndef PROCESS_H
#define PROCESS_H

#include "common.h"

#define MAX_ARGS_EXEC 16

/* Load an ELF executable from the file system into a brand-new address
 * space and start it in user mode. Returns the new pid or a negative error. */
int  process_spawn(const char *path, int argc, char *const argv[], int priority);

/* System call argument checking: is [p, p+n) user memory that the calling
 * process really owns? Always true for kernel tasks. */
bool user_ok(const void *p, uint32_t n);
/* Safely copy a NUL-terminated string from user memory. -E_FAULT if bad. */
int  copy_user_str(char *dst, const char *src, uint32_t max);

#endif
