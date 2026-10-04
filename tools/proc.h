/*
 * Just enough process control for the crash test: start a copy of this
 * program with its stdout connected to a pipe, read its output line by line,
 * and kill it without warning (SIGKILL / TerminateProcess) - no atexit
 * handlers, no flushing, no chance to clean up.
 */
#ifndef TS_PROC_H
#define TS_PROC_H

#include <stddef.h>

typedef struct proc proc;

/* POSIX: fork()s and runs child_main(argc, argv) in the child.
   Windows: starts this executable again with argv as its arguments, and the
   program's main must dispatch to child_main itself. */
int  proc_spawn_self(proc **out, int argc, char **argv, int (*child_main)(int, char **));
/* Reads one complete line (without the newline). Returns its length, or 0 at end of output. */
int  proc_read_line(proc *p, char *buf, size_t size);
void proc_kill(proc *p);
void proc_free(proc *p);
void sleep_ms(unsigned ms);

#endif
