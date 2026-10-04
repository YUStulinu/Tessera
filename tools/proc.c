#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

struct proc {
    HANDLE process;
    HANDLE out;  /* read end of the child's stdout */
    char   buf[4096];
    size_t len, pos;
};

int proc_spawn_self(proc **out, int argc, char **argv, int (*child_main)(int, char **)) {
    (void)child_main;
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) return -1;
    char cmd[4096];
    int n = snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    for (int i = 0; i < argc && n > 0 && (size_t)n < sizeof cmd; i++)
        n += snprintf(cmd + n, sizeof cmd - (size_t)n, " \"%s\"", argv[i]);

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); /* only the write end goes to the child */

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(wr); /* the parent's copy; reads see end-of-file once the child dies */
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    CloseHandle(pi.hThread);
    proc *p = (proc *)calloc(1, sizeof *p);
    if (!p) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(rd);
        return -1;
    }
    p->process = pi.hProcess;
    p->out = rd;
    *out = p;
    return 0;
}

static int fill(proc *p) {
    DWORD got = 0;
    if (!ReadFile(p->out, p->buf, sizeof p->buf, &got, NULL) || got == 0) return 0;
    p->len = got;
    p->pos = 0;
    return 1;
}

void proc_kill(proc *p) {
    TerminateProcess(p->process, 9);
    WaitForSingleObject(p->process, INFINITE);
}

void proc_free(proc *p) {
    if (!p) return;
    CloseHandle(p->process);
    CloseHandle(p->out);
    free(p);
}

void sleep_ms(unsigned ms) {
    Sleep(ms);
}

#else
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct proc {
    pid_t  pid;
    int    out;
    char   buf[4096];
    size_t len, pos;
};

int proc_spawn_self(proc **out, int argc, char **argv, int (*child_main)(int, char **)) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        _exit(child_main(argc, argv));
    }
    close(fds[1]);
    proc *p = (proc *)calloc(1, sizeof *p);
    if (!p) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(fds[0]);
        return -1;
    }
    p->pid = pid;
    p->out = fds[0];
    *out = p;
    return 0;
}

static int fill(proc *p) {
    for (;;) {
        ssize_t got = read(p->out, p->buf, sizeof p->buf);
        if (got > 0) {
            p->len = (size_t)got;
            p->pos = 0;
            return 1;
        }
        if (got < 0 && errno == EINTR) continue;
        return 0;
    }
}

void proc_kill(proc *p) {
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
}

void proc_free(proc *p) {
    if (!p) return;
    close(p->out);
    free(p);
}

void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

int proc_read_line(proc *p, char *buf, size_t size) {
    size_t n = 0;
    buf[0] = '\0';
    for (;;) {
        /* A line cut off by the child's death is not a line: a half-written
           "123" must not be mistaken for an acknowledgement of "12". */
        if (p->pos == p->len && !fill(p)) return 0;
        char c = p->buf[p->pos++];
        if (c == '\n') {
            if (n == 0) continue; /* skip empty lines */
            return (int)n;
        }
        if (c != '\r' && n + 1 < size) {
            buf[n++] = c;
            buf[n] = '\0';
        }
    }
}
