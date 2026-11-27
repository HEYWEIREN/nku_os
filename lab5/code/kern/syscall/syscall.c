#include <unistd.h>
#include <proc.h>
#include <syscall.h>
#include <trap.h>
#include <stdio.h>
#include <pmm.h>
#include <assert.h>


//lab5 function1 2410867
#include <error.h>

static int sys_exit(uint64_t arg[])
{
    return do_exit((int)arg[0]);
}

static int sys_fork(uint64_t arg[])
{
    struct trapframe *tf = current->tf;
    return do_fork(0, tf->gpr.sp, tf);
}

static int sys_wait(uint64_t arg[])
{
    return do_wait((int)arg[0], (int *)arg[1]);
}

static int sys_exec(uint64_t arg[])
{
    return do_execve((const char *)arg[0], (size_t)arg[1],
                     (unsigned char *)arg[2], (size_t)arg[3]);
}

static int sys_yield(uint64_t arg[])
{
    return do_yield();
}

static int sys_kill(uint64_t arg[])
{
    return do_kill((int)arg[0]);
}

static int sys_getpid(uint64_t arg[])
{
    return current->pid;
}

static int sys_putc(uint64_t arg[])
{
    cputchar((int)arg[0]);
    return 0;
}

static int sys_pgdir(uint64_t arg[])
{
    print_pgdir();
    return 0;
}

static int (*syscalls[])(uint64_t arg[]) = {
    [SYS_exit]              sys_exit,
    [SYS_fork]              sys_fork,
    [SYS_wait]              sys_wait,
    [SYS_exec]              sys_exec,
    [SYS_yield]             sys_yield,
    [SYS_kill]              sys_kill,
    [SYS_getpid]            sys_getpid,
    [SYS_putc]              sys_putc,
    [SYS_pgdir]             sys_pgdir,
};

#define NUM_SYSCALLS        ((sizeof(syscalls)) / (sizeof(syscalls[0])))

void syscall(void)
{
    struct trapframe *tf = current->tf;
    uint64_t number = tf->gpr.a0;
    if (number >= NUM_SYSCALLS || syscalls[number] == NULL) {
        tf->gpr.a0 = -E_INVAL;
        return;
    }

    uint64_t args[] = {tf->gpr.a1, tf->gpr.a2, tf->gpr.a3, tf->gpr.a4, tf->gpr.a5};
    tf->gpr.a0 = syscalls[number](args);
}
