#include <proc.h>
#include <kmalloc.h>
#include <string.h>
#include <sync.h>
#include <pmm.h>
#include <error.h>
#include <sched.h>
#include <elf.h>
#include <vmm.h>
#include <trap.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

/* ------------- process/thread mechanism design&implementation -------------
(an simplified Linux process/thread mechanism )
introduction:
  ucore implements a simple process/thread mechanism. process contains the independent memory sapce, at least one threads
for execution, the kernel data(for management), processor state (for context switch), files(in lab6), etc. ucore needs to
manage all these details efficiently. In ucore, a thread is just a special kind of process(share process's memory).
------------------------------
process state       :     meaning               -- reason
    PROC_UNINIT     :   uninitialized           -- alloc_proc
    PROC_SLEEPING   :   sleeping                -- try_free_pages, do_wait, do_sleep
    PROC_RUNNABLE   :   runnable(maybe running) -- proc_init, wakeup_proc,
    PROC_ZOMBIE     :   almost dead             -- do_exit

-----------------------------
process state changing:

  alloc_proc                                 RUNNING
      +                                   +--<----<--+
      +                                   + proc_run +
      V                                   +-->---->--+
PROC_UNINIT -- proc_init/wakeup_proc --> PROC_RUNNABLE -- try_free_pages/do_wait/do_sleep --> PROC_SLEEPING --
                                           A      +                                                           +
                                           |      +--- do_exit --> PROC_ZOMBIE                                +
                                           +                                                                  +
                                           -----------------------wakeup_proc----------------------------------
-----------------------------
process relations
parent:           proc->parent  (proc is children)
children:         proc->cptr    (proc is parent)
older sibling:    proc->optr    (proc is younger sibling)
younger sibling:  proc->yptr    (proc is older sibling)
-----------------------------
related syscall for process:
SYS_exit        : process exit,                           -->do_exit
SYS_fork        : create child process, dup mm            -->do_fork-->wakeup_proc
SYS_wait        : wait process                            -->do_wait
SYS_exec        : after fork, process execute a program   -->load a program and refresh the mm
SYS_clone       : create child thread                     -->do_fork-->wakeup_proc
SYS_yield       : process flag itself need resecheduling, -- proc->need_sched=1, then scheduler will rescheule this process
SYS_sleep       : process sleep                           -->do_sleep
SYS_kill        : kill process                            -->do_kill-->proc->flags |= PF_EXITING
                                                                 -->wakeup_proc-->do_wait-->do_exit
SYS_getpid      : get the process's pid

*/

// the process set's list
list_entry_t proc_list;

#define HASH_SHIFT 10
#define HASH_LIST_SIZE (1 << HASH_SHIFT)
#define pid_hashfn(x) (hash32(x, HASH_SHIFT))

// has list for process set based on pid
static list_entry_t hash_list[HASH_LIST_SIZE];

// idle proc
struct proc_struct *idleproc = NULL;
// init proc
struct proc_struct *initproc = NULL;
// current proc
struct proc_struct *current = NULL;

static int nr_process = 0;

void kernel_thread_entry(void);
void forkrets(struct trapframe *tf);
void switch_to(struct context *from, struct context *to);

//lab4 请补充你在lab4中的代码
static void
switch_pgdir(uintptr_t pgdir_pa)
{
    uintptr_t satp = ((uintptr_t)SATP_MODE_SV39 << 60) | (pgdir_pa >> RISCV_PGSHIFT);
    write_csr(satp, satp);
    asm volatile("sfence.vma" ::: "memory");
}

void
cpu_idle(void)
{
    while (1)
    {
        if (current->need_resched)
        {
            schedule();
        }
    }
}

void
proc_run(struct proc_struct *proc)
{
    if (proc != current)
    {
        bool intr_flag;
        local_intr_save(intr_flag);
        {
            struct proc_struct *prev = current;
            current = proc;
            switch_pgdir(proc->pgdir);
            switch_to(&(prev->context), &(proc->context));
        }
        local_intr_restore(intr_flag);
    }
}

//lab5 function2 2410867 update alloc_proc
static struct proc_struct *
alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc != NULL)
    {
        memset(proc, 0, sizeof(struct proc_struct));
        proc->state = PROC_UNINIT;
        proc->pid = -1;
        proc->pgdir = boot_pgdir_pa;
        list_init(&(proc->list_link));
        list_init(&(proc->hash_link));
    }
    return proc;
}


// set_proc_name - set the name of proc
char *
set_proc_name(struct proc_struct *proc, const char *name)
{
    memset(proc->name, 0, sizeof(proc->name));
    return memcpy(proc->name, name, PROC_NAME_LEN);
}

// get_proc_name - get the name of proc
char *
get_proc_name(struct proc_struct *proc)
{
    static char name[PROC_NAME_LEN + 1];
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->name, PROC_NAME_LEN);
}

// set_links - set the relation links of process
static void
set_links(struct proc_struct *proc)
{
    list_add(&proc_list, &(proc->list_link));
    proc->yptr = NULL;
    if ((proc->optr = proc->parent->cptr) != NULL)
    {
        proc->optr->yptr = proc;
    }
    proc->parent->cptr = proc;
    nr_process++;
}

// remove_links - clean the relation links of process
static void
remove_links(struct proc_struct *proc)
{
    list_del(&(proc->list_link));
    if (proc->optr != NULL)
    {
        proc->optr->yptr = proc->yptr;
    }
    if (proc->yptr != NULL)
    {
        proc->yptr->optr = proc->optr;
    }
    else
    {
        proc->parent->cptr = proc->optr;
    }
    nr_process--;
}

// get_pid - alloc a unique pid for process
static int
get_pid(void)
{
    static_assert(MAX_PID > MAX_PROCESS);
    struct proc_struct *proc;
    list_entry_t *list = &proc_list, *le;
    static int next_safe = MAX_PID, last_pid = MAX_PID;
    if (++last_pid >= MAX_PID)
    {
        last_pid = 1;
        goto inside;
    }
    if (last_pid >= next_safe)
    {
    inside:
        next_safe = MAX_PID;
    repeat:
        le = list;
        while ((le = list_next(le)) != list)
        {
            proc = le2proc(le, list_link);
            if (proc->pid == last_pid)
            {
                if (++last_pid >= next_safe)
                {
                    if (last_pid >= MAX_PID)
                    {
                        last_pid = 1;
                    }
                    next_safe = MAX_PID;
                    goto repeat;
                }
            }
            else if (proc->pid > last_pid && next_safe > proc->pid)
            {
                next_safe = proc->pid;
            }
        }
    }
    return last_pid;
}






// forkret -- the first kernel entry point of a new thread/process
// NOTE: the addr of forkret is setted in copy_thread function
//       after switch_to, the current proc will execute here.
static void
forkret(void)
{
    forkrets(current->tf);
}

// hash_proc - add proc into proc hash_list
static void
hash_proc(struct proc_struct *proc)
{
    list_add(hash_list + pid_hashfn(proc->pid), &(proc->hash_link));
}

// unhash_proc - delete proc from proc hash_list
static void
unhash_proc(struct proc_struct *proc)
{
    list_del(&(proc->hash_link));
}

// find_proc - find proc frome proc hash_list according to pid
struct proc_struct *
find_proc(int pid)
{
    if (0 < pid && pid < MAX_PID)
    {
        list_entry_t *list = hash_list + pid_hashfn(pid), *le = list;
        while ((le = list_next(le)) != list)
        {
            struct proc_struct *proc = le2proc(le, hash_link);
            if (proc->pid == pid)
            {
                return proc;
            }
        }
    }
    return NULL;
}

//lab5 function2 2410867 update kernel_thread
int
kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags)
{
    struct trapframe tf;
    memset(&tf, 0, sizeof(struct trapframe));
    tf.gpr.s0 = (uintptr_t)fn;
    tf.gpr.s1 = (uintptr_t)arg;
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;
    tf.epc = (uintptr_t)kernel_thread_entry;
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}


// setup_kstack - alloc pages with size KSTACKPAGE as process kernel stack
static int
setup_kstack(struct proc_struct *proc)
{
    struct Page *page = alloc_pages(KSTACKPAGE);
    if (page != NULL)
    {
        proc->kstack = (uintptr_t)page2kva(page);
        return 0;
    }
    return -E_NO_MEM;
}

// put_kstack - free the memory space of process kernel stack
static void
put_kstack(struct proc_struct *proc)
{
    free_pages(kva2page((void *)(proc->kstack)), KSTACKPAGE);
}

// setup_pgdir - alloc one page as PDT
static int
setup_pgdir(struct mm_struct *mm)
{
    struct Page *page;
    if ((page = alloc_page()) == NULL)
    {
        return -E_NO_MEM;
    }
    pde_t *pgdir = page2kva(page);
    memcpy(pgdir, boot_pgdir_va, PGSIZE);

    mm->pgdir = pgdir;
    return 0;
}

// put_pgdir - free the memory space of PDT
static void
put_pgdir(struct mm_struct *mm)
{
    free_page(kva2page(mm->pgdir));
}

//lab5 function3 2410867 update copy_mm
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc)
{
    struct mm_struct *old_mm = current->mm;
    if (old_mm == NULL) {
        return 0;
    }

    struct mm_struct *mm = old_mm;
    if (!(clone_flags & CLONE_VM)) {
        mm = mm_create();
        if (mm == NULL) {
            return -E_NO_MEM;
        }
        int ret = setup_pgdir(mm);
        if (ret != 0) {
            mm_destroy(mm);
            return ret;
        }
        lock_mm(old_mm);
        ret = dup_mmap(mm, old_mm);
        unlock_mm(old_mm);
        if (ret != 0) {
            exit_mmap(mm);
            put_pgdir(mm);
            mm_destroy(mm);
            return ret;
        }
        asm volatile("fence.i" ::: "memory");
    }

    mm_count_inc(mm);
    proc->mm = mm;
    proc->pgdir = PADDR(mm->pgdir);
    return 0;
}



//lab5 function2  2410867
static bool
process_buffer_access(uintptr_t start, size_t len, bool write)
{
    if (len == 0) {
        return 1;
    }
    struct mm_struct *mm = current->mm;
    if (!user_mem_check(mm, start, len, write)) {
        return 0;
    }
    if (mm == NULL) {
        return 1;
    }

    uint32_t required = PTE_V | PTE_U | (write ? PTE_W : PTE_R);
    uintptr_t end = start + len;
    for (uintptr_t va = ROUNDDOWN(start, PGSIZE); va < end; va += PGSIZE) {
        pte_t *pte = get_pte(mm->pgdir, va, 0);
        if (pte == NULL || (*pte & required) != required) {
            return 0;
        }
    }
    return 1;
}

static void
release_current_mm(void)
{
    struct mm_struct *mm = current->mm;
    if (mm == NULL) {
        return;
    }
    current->mm = NULL;
    current->pgdir = boot_pgdir_pa;
    switch_pgdir(boot_pgdir_pa);
    if (mm_count_dec(mm) == 0) {
        exit_mmap(mm);
        put_pgdir(mm);
        mm_destroy(mm);
    }
}

static int
load_icode(unsigned char *binary, size_t size)
{
    struct elfhdr elf;
    if (size < sizeof(elf)) {
        return -E_INVAL_ELF;
    }
    memcpy(&elf, binary, sizeof(elf));
    if (elf.e_magic != ELF_MAGIC || elf.e_elf[0] != 2 || elf.e_elf[1] != 1 ||
        elf.e_phentsize != sizeof(struct proghdr) || elf.e_phoff > size ||
        elf.e_phnum > (size - elf.e_phoff) / sizeof(struct proghdr)) {
        return -E_INVAL_ELF;
    }

    struct mm_struct *mm = mm_create();
    if (mm == NULL) {
        return -E_NO_MEM;
    }
    int ret = setup_pgdir(mm);
    if (ret != 0) {
        goto bad_mm;
    }

    for (size_t i = 0; i < elf.e_phnum; i++) {
        struct proghdr ph;
        memcpy(&ph, binary + elf.e_phoff + i * sizeof(ph), sizeof(ph));
        if (ph.p_type != ELF_PT_LOAD) {
            continue;
        }
        if (ph.p_filesz > ph.p_memsz || ph.p_offset > size ||
            ph.p_filesz > size - ph.p_offset) {
            ret = -E_INVAL_ELF;
            goto bad_mmap;
        }
        if (ph.p_memsz == 0) {
            continue;
        }
        if (ph.p_va < USERBASE || ph.p_va >= USTACKTOP - USTACKSIZE ||
            ph.p_memsz > USTACKTOP - USTACKSIZE - ph.p_va) {
            ret = -E_INVAL_ELF;
            goto bad_mmap;
        }

        uint32_t vm_flags = 0, perm = PTE_U;
        if (ph.p_flags & (ELF_PF_R | ELF_PF_W)) {
            vm_flags |= VM_READ;
            perm |= PTE_R;
        }
        if (ph.p_flags & ELF_PF_W) {
            vm_flags |= VM_WRITE;
            perm |= PTE_W;
        }
        if (ph.p_flags & ELF_PF_X) {
            vm_flags |= VM_EXEC;
            perm |= PTE_X;
        }
        if (vm_flags == 0) {
            ret = -E_INVAL_ELF;
            goto bad_mmap;
        }
        uintptr_t segment_start = ROUNDDOWN(ph.p_va, PGSIZE);
        uintptr_t segment_end = ROUNDUP(ph.p_va + ph.p_memsz, PGSIZE);
        list_entry_t *list = &mm->mmap_list;
        for (list_entry_t *le = list_next(list); le != list; le = list_next(le)) {
            struct vma_struct *vma = le2vma(le, list_link);
            if (segment_start < vma->vm_end && vma->vm_start < segment_end) {
                ret = -E_INVAL_ELF;
                goto bad_mmap;
            }
        }
        ret = mm_map(mm, ph.p_va, ph.p_memsz, vm_flags, NULL);
        if (ret != 0) {
            goto bad_mmap;
        }

        uintptr_t file_end = ph.p_va + ph.p_filesz;
        for (uintptr_t va = segment_start; va < segment_end; va += PGSIZE) {
            struct Page *page = pgdir_alloc_page(mm->pgdir, va, perm);
            if (page == NULL) {
                ret = -E_NO_MEM;
                goto bad_mmap;
            }
            unsigned char *dst = page2kva(page);
            memset(dst, 0, PGSIZE);
            uintptr_t first = va < ph.p_va ? ph.p_va : va;
            uintptr_t end = (va + PGSIZE < file_end) ? va + PGSIZE : file_end;
            if (first < end) {
                memcpy(dst + (first - va), binary + ph.p_offset + (first - ph.p_va),
                       end - first);
            }
        }
    }

    if (elf.e_entry < USERBASE || elf.e_entry >= USTACKTOP - USTACKSIZE) {
        ret = -E_INVAL_ELF;
        goto bad_mmap;
    }
    pte_t *entry = get_pte(mm->pgdir, elf.e_entry, 0);
    if (entry == NULL || (*entry & (PTE_V | PTE_X)) != (PTE_V | PTE_X)) {
        ret = -E_INVAL_ELF;
        goto bad_mmap;
    }

    ret = mm_map(mm, USTACKTOP - USTACKSIZE, USTACKSIZE, VM_READ | VM_WRITE | VM_STACK, NULL);
    if (ret != 0) {
        goto bad_mmap;
    }
    for (uintptr_t va = USTACKTOP - 4 * PGSIZE; va < USTACKTOP; va += PGSIZE) {
        struct Page *page = pgdir_alloc_page(mm->pgdir, va, PTE_U | PTE_R | PTE_W);
        if (page == NULL) {
            ret = -E_NO_MEM;
            goto bad_mmap;
        }
        memset(page2kva(page), 0, PGSIZE);
    }

    mm_count_inc(mm);
    current->mm = mm;
    current->pgdir = PADDR(mm->pgdir);
    switch_pgdir(current->pgdir);
    asm volatile("fence.i" ::: "memory");

    struct trapframe *tf = current->tf;
    memset(tf, 0, sizeof(*tf));
    tf->gpr.sp = USTACKTOP;
    tf->epc = elf.e_entry;
    tf->status = (read_csr(sstatus) | SSTATUS_SPIE | SSTATUS_SUM) &
                 ~(SSTATUS_SPP | SSTATUS_SIE);
    return 0;

bad_mmap:
    exit_mmap(mm);
    put_pgdir(mm);
bad_mm:
    mm_destroy(mm);
    return ret;
}

int
do_execve(const char *name, size_t len, unsigned char *binary, size_t size)
{
    char local_name[PROC_NAME_LEN + 1] = {0};
    if (len > PROC_NAME_LEN) {
        len = PROC_NAME_LEN;
    }
    if (size == 0 || !process_buffer_access((uintptr_t)name, len, 0) ||
        !process_buffer_access((uintptr_t)binary, size, 0)) {
        return -E_INVAL;
    }
    if (len != 0) {
        memcpy(local_name, name, len);
    }

    struct Page *image = NULL;
    size_t image_pages = 0;
    if (current->mm != NULL) {
        image_pages = ROUNDUP(size, PGSIZE) / PGSIZE;
        image = alloc_pages(image_pages);
        if (image == NULL) {
            return -E_NO_MEM;
        }
        void *image_data = page2kva(image);
        memcpy(image_data, binary, size);
        binary = image_data;
    }

    release_current_mm();
    int ret = load_icode(binary, size);
    if (image != NULL) {
        free_pages(image, image_pages);
    }
    if (ret != 0) {
        do_exit(ret);
    }
    set_proc_name(current, local_name);
    return 0;
}

int
do_wait(int pid, int *code_store)
{
    if (code_store != NULL && !process_buffer_access((uintptr_t)code_store,
                                                      sizeof(*code_store), 1)) {
        return -E_INVAL;
    }

    for (;;) {
        bool intr_flag;
        local_intr_save(intr_flag);
        if (current->flags & PF_EXITING) {
            do_exit(-E_KILLED);
        }

        struct proc_struct *child = (pid == 0) ? current->cptr : find_proc(pid);
        if (child == NULL || child->parent != current) {
            local_intr_restore(intr_flag);
            return -E_BAD_PROC;
        }
        if (pid == 0) {
            while (child != NULL && child->state != PROC_ZOMBIE) {
                child = child->optr;
            }
        }
        if (child != NULL && child->state == PROC_ZOMBIE) {
            if (code_store != NULL) {
                memcpy(code_store, &child->exit_code, sizeof(*code_store));
            }
            unhash_proc(child);
            remove_links(child);
            local_intr_restore(intr_flag);
            put_kstack(child);
            kfree(child);
            return 0;
        }

        current->state = PROC_SLEEPING;
        current->wait_state = WT_CHILD;
        schedule();
        local_intr_restore(intr_flag);
    }
}


//lab5 function3 2410867
static void
copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf)
{
    proc->tf = (struct trapframe *)(proc->kstack + KSTACKSIZE - sizeof(struct trapframe));
    *(proc->tf) = *tf;
    proc->tf->gpr.a0 = 0;
    proc->tf->gpr.sp = trap_in_kernel(tf) ? (uintptr_t)proc->tf : esp;
    proc->context.ra = (uintptr_t)forkret;
    proc->context.sp = (uintptr_t)proc->tf;
}

int
do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf)
{
    int ret = -E_NO_FREE_PROC;
    if (nr_process >= MAX_PROCESS)
    {
        return ret;
    }

    ret = -E_NO_MEM;
    struct proc_struct *proc = alloc_proc();
    if (proc == NULL)
    {
        return ret;
    }
    proc->parent = current;

    if ((ret = setup_kstack(proc)) != 0)
    {
        goto bad_fork_cleanup_proc;
    }
    if ((ret = copy_mm(clone_flags, proc)) != 0)
    {
        goto bad_fork_cleanup_kstack;
    }
    copy_thread(proc, stack, tf);

    bool intr_flag;
    local_intr_save(intr_flag);
    {
        proc->pid = get_pid();
        hash_proc(proc);
        set_links(proc);
        // Publish a complete PCB before it becomes runnable.
        ret = proc->pid;
        wakeup_proc(proc);
    }
    local_intr_restore(intr_flag);
    return ret;

bad_fork_cleanup_kstack:
    put_kstack(proc);
bad_fork_cleanup_proc:
    kfree(proc);
    return ret;
}


//lab5 function4  2413802
int
do_exit(int error_code)
{
    if (current == idleproc || current == initproc) {
        panic("idle and init cannot exit");
    }

    bool intr_flag;
    local_intr_save(intr_flag);
    release_current_mm();
    current->state = PROC_ZOMBIE;
    current->exit_code = error_code;
    struct proc_struct *parent = current->parent;
    if (parent->wait_state == WT_CHILD) {
        wakeup_proc(parent);
    }

    while (current->cptr != NULL) {
        struct proc_struct *child = current->cptr;
        current->cptr = child->optr;
        if (current->cptr != NULL) {
            current->cptr->yptr = NULL;
        }
        child->parent = initproc;
        child->yptr = NULL;
        child->optr = initproc->cptr;
        if (child->optr != NULL) {
            child->optr->yptr = child;
        }
        initproc->cptr = child;
        if (child->state == PROC_ZOMBIE && initproc->wait_state == WT_CHILD) {
            wakeup_proc(initproc);
        }
    }
    schedule();
    local_intr_restore(intr_flag);
    panic("zombie process was rescheduled");
}

int
do_yield(void)
{
    current->need_resched = 1;
    return 0;
}

int
do_kill(int pid)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    struct proc_struct *proc = find_proc(pid);
    int ret = -E_INVAL;
    if (proc != NULL && proc != initproc && proc->state != PROC_ZOMBIE) {
        ret = -E_KILLED;
        if (!(proc->flags & PF_EXITING)) {
            proc->flags |= PF_EXITING;
            if (proc->wait_state & WT_INTERRUPTED) {
                wakeup_proc(proc);
            }
            ret = 0;
        }
    }
    local_intr_restore(intr_flag);
    return ret;
}


#define __KERNEL_EXECVE(name, binary, size) ({           \
    cprintf("kernel_execve: pid = %d, name = \"%s\".\n", \
            current->pid, name);                         \
    kernel_execve(name, binary, (size_t)(size));         \
})

#define KERNEL_EXECVE(x) ({                                    \
    extern unsigned char _binary_obj___user_##x##_out_start[], \
        _binary_obj___user_##x##_out_size[];                   \
    __KERNEL_EXECVE(#x, _binary_obj___user_##x##_out_start,    \
                    _binary_obj___user_##x##_out_size);        \
})

#define __KERNEL_EXECVE2(x, xstart, xsize) ({   \
    extern unsigned char xstart[], xsize[];     \
    __KERNEL_EXECVE(#x, xstart, (size_t)xsize); \
})

#define KERNEL_EXECVE2(x, xstart, xsize) __KERNEL_EXECVE2(x, xstart, xsize)


//lab5 function2 2410867
#ifdef DEBUG_GRADE
#include <kmonitor.h>
#endif

static int
kernel_execve(const char *name, unsigned char *binary, size_t size)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    current->tf = (struct trapframe *)(current->kstack + KSTACKSIZE - sizeof(struct trapframe));
    int ret = do_execve(name, strlen(name), binary, size);
    if (ret != 0) {
        local_intr_restore(intr_flag);
        return ret;
    }
    forkrets(current->tf);
    panic("forkrets returned after kernel_execve");
}

static int
user_main(void *arg)
{
#ifdef TEST
    return KERNEL_EXECVE2(TEST, TESTSTART, TESTSIZE);
#else
    return KERNEL_EXECVE(exit);
#endif
}

static int
init_main(void *arg)
{
    size_t free_bytes_before = nr_free_pages() * PGSIZE + kmalloc_free_bytes();
    int pid = kernel_thread(user_main, NULL, 0);
    if (pid <= 0) {
        panic("cannot create user_main: %d", pid);
    }

    while (do_wait(0, NULL) == 0) {
        /* Reap the first user process and any descendants adopted by init. */
    }
    cprintf("all user-mode processes have quit.\n");
    assert(initproc->cptr == NULL && list_next(&proc_list) == &initproc->list_link &&
           list_next(&initproc->list_link) == &proc_list && nr_process == 2);
    assert(nr_free_pages() * PGSIZE + kmalloc_free_bytes() == free_bytes_before);
    cprintf("init check memory pass.\n");

#ifdef DEBUG_GRADE
    kmonitor(NULL);
#endif

    bool intr_flag;
    local_intr_save(intr_flag);
    current->state = PROC_SLEEPING;
    schedule();
    local_intr_restore(intr_flag);
    panic("init was unexpectedly rescheduled");
}

void
proc_init(void){
    list_init(&proc_list);
    for (int i = 0; i < HASH_LIST_SIZE; i++)
    {
        list_init(hash_list + i);
    }

    idleproc = alloc_proc();
    if (idleproc == NULL)
    {
        panic("cannot allocate idleproc.\n");
    }
    idleproc->pid = 0;
    idleproc->state = PROC_RUNNABLE;
    idleproc->kstack = (uintptr_t)bootstack;
    idleproc->need_resched = 1;
    // set_proc_name copies PROC_NAME_LEN bytes from its source.
    static const char idle_name[PROC_NAME_LEN + 1] = "idle";
    static const char init_name[PROC_NAME_LEN + 1] = "init";
    set_proc_name(idleproc, idle_name);
    current = idleproc;
    nr_process++;

    int pid = kernel_thread(init_main, NULL, 0);
    if (pid <= 0)
    {
        panic("cannot create initproc: %d.\n", pid);
    }
    initproc = find_proc(pid);
    assert(initproc != NULL && initproc->pid == 1);
    set_proc_name(initproc, init_name);
}



