#include <clock.h>
#include <console.h>
#include <defs.h>
#include <intr.h>
#include <kdebug.h>
#include <kmonitor.h>
#include <pmm.h>
#include <stdio.h>
#include <string.h>
#include <trap.h>
#include <dtb.h>

int kern_init(void) __attribute__((noreturn));
void grade_backtrace(void);

int kern_init(void) {
    extern char edata[], end[];
    // 先清零 BSS，再读取并保存 DTB 的内存信息，避免被清零覆盖（为了解释变化 正式上传时我觉得应该删去这句话）
    memset(edata, 0, end - edata);
    dtb_init();
    cons_init();  // init the console
    const char *message = "(THU.CST) os is loading ...\0";
    //cprintf("%s\n\n", message);
    cputs(message);

    print_kerninfo();

    // grade_backtrace();
    
    //IDT）就是“中断描述符表”，它是一张供 CPU 查询的表：不同中断或异常对应不同编号，
    //CPU 根据编号找到描述符，再取得处理入口等信息。
    idt_init();  // init interrupt descriptor table

    pmm_init();  // init physical memory management

    idt_init();  // init interrupt descriptor table

    clock_init();   // init clock interrupt
    /*
    STIP 是状态标志：有没有请求在等待。（运行在 M 模式的 OpenSBI来置位）
    STIE 是分类开关：允不允许时钟中断。
    SIE 是总开关：在当前内核运行于 S 模式的情况下，允不允许响应中断。
    */
    intr_enable();  // enable irq interrupt
    /*
    IRQ = Interrupt Request，即“中断请求”。
    当定时器到期、键盘有输入或设备完成操作时，硬件可以发出中断请求，
    让 CPU 暂时停下当前程序，转去执行相应的处理代码，处理完再继续原来的程序。
    在 Lab3 中，主要接触的是时钟中断请求
    */

    /* do nothing */
    while (1)
        ;
}

void __attribute__((noinline))
grade_backtrace2(int arg0, int arg1, int arg2, int arg3) {
    mon_backtrace(0, NULL, NULL);
}

void __attribute__((noinline)) grade_backtrace1(int arg0, int arg1) {
    grade_backtrace2(arg0, (uintptr_t)&arg0, arg1, (uintptr_t)&arg1);
}

void __attribute__((noinline)) grade_backtrace0(int arg0, int arg1, int arg2) {
    grade_backtrace1(arg0, arg2);
}

void grade_backtrace(void) { grade_backtrace0(0, (uintptr_t)kern_init, 0xffff0000); }

/*
前提：初始化已经完成
────────────────────────────────────────────
OpenSBI 准备：
  mtvec          → OpenSBI 的 M 模式入口
  mie.MTIE = 1   → 允许 M 模式时钟中断
  mideleg[5] = 1 → 将 S 模式时钟中断委托给 S 模式
  M 模式时钟中断仍由 M 模式处理

Lab3 内核准备：
  stvec          → 内核的 __alltraps 入口
  sie.STIE = 1   → 允许 S 模式时钟中断
  sstatus.SIE = 1→ 开启当前 S 模式的全局中断
  已通过 OpenSBI 设置定时目标
────────────────────────────────────────────
                    ↓
① CPU 正在 S 模式执行内核，定时器达到目标时间
   硬件使 mip.MTIP = 1
                    ↓
② CPU 判断：现在能否响应 M 模式时钟中断？

   检查 A：mip.MTIP == 1？
           是，有请求

   检查 B：mie.MTIE == 1？
           是，这一类中断已开启

   检查 C：当前特权级允许进入 M 模式处理吗？
           当前是 S，低于 M → 允许
           此时不要求 mstatus.MIE == 1

   任一必要条件不满足 → 暂不响应，继续原程序
   条件满足 ↓

   CPU 自动：
     保存返回位置到 mepc，记录原因到 mcause
     更新 mstatus，切换到 M 模式
     跳到 mtvec 指定的 OpenSBI 入口
                    ↓
③ CPU 在 M 模式执行 OpenSBI
   OpenSBI 处理底层定时事件
   设置 mip.STIP = 1
   对应的 sip.STIP 也反映出待处理状态

   此时 CPU 仍在 M 模式：
     S 模式中断不能打断 M 模式执行
     所以 STIP 请求先等待
                    ↓
④ OpenSBI 返回，CPU 恢复到原来的 S 模式
                    ↓
⑤ CPU 判断：现在能否响应 S 模式时钟中断？

   检查 A：sip.STIP == 1？
           是，有请求

   检查 B：mideleg[5] == 1？
           是，这类中断已委托给 S 模式
           若不是 1，就不会按 S 模式陷阱路径进入内核

   检查 C：sie.STIE == 1？
           是，这一类中断已开启

   检查 D：当前是 S 模式，sstatus.SIE == 1？
           是，当前模式的全局中断已开启

   若使能条件不满足 → 暂不响应，请求可以保持待处理
   条件满足 ↓

   CPU 自动：
     保存返回位置到 sepc，记录原因到 scause
     保存原中断开关状态，并将 SIE 清零
     记录原特权级，当前仍为 S 模式
     跳到 stvec 指定的 __alltraps
                    ↓
⑥ CPU 执行内核中断处理代码
   保存现场 → 处理时钟事件 → 恢复现场 → sret 返回
*/