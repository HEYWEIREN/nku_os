# Lab1：最小内核与启动流程

## 实验基本信息

- 当前记录日期：2026-10-08。
- 实验成员与小组分工：待填写。当前操作记录来自 2413802-Xing Taoqing。
- 状态：练习1和练习2的核心调试步骤已有本轮终端证据；已确认本轮内核启动输出，第五节已整理实际终端记录；小组信息待填写。
- 本报告仅使用本轮正式调试记录，不将之前试跑作为本次实验结果。

## 一、实验目的

理解最小内核的构建、加载与启动，解释汇编入口如何建立栈并转入 C 函数，使用 QEMU 和 GDB 验证从复位到内核入口的执行过程。

## 二、实验环境

- Ubuntu 22.04 / WSL；QEMU 6.2.0；OpenSBI v0.9。
- RISC-V 交叉编译工具链：riscv64-unknown-elf-*。
- 调试器：GNU GDB 14.2，目标 riscv64-unknown-elf。
- AI 工具：Codex；具体模型版本、小组其他成员使用情况待按实际填写。
- 在 lab1/code 目录内构建与运行。

## 三、实验整体逻辑分析

构建阶段：源文件编译为目标文件，链接器根据 tools/kernel.ld 生成 bin/kernel，再由 objcopy 生成 bin/ucore.img。

启动阶段：QEMU 准备模拟机器并加载固件与内核；CPU 从复位入口执行，进入 OpenSBI，随后由固件初始化环境并向 S 模式内核交接。内核从 kern_entry 设置栈，再进入 kern_init 清零 BSS、打印启动信息并循环等待。

加载镜像与执行镜像是不同的步骤。链接地址、实际加载位置与固件的下一阶段入口需要一致。

## 四、实验内容与实现

### 环境适配

当前 Makefile 将原来的 -device loader,file=$(UCOREIMG),addr=0x80200000 保留为注释，qemu 和 debug 目标均使用 -kernel $(UCOREIMG)。原参数在本机默认固件下出现 Next Address 为 0 的问题；新参数用于向默认固件提供正确的内核启动信息。内核源码未因该适配而修改。

### 练习1：理解内核启动中的程序入口操作

#### la sp, bootstacktop

将 bootstacktop 的地址装入栈指针 sp，为 C 代码的局部变量、寄存器保存和函数调用建立内核栈。la 是伪指令，可能展开为多条机器指令。

本轮观察：

```text
命中 kern_entry：pc = 0x80200000，sp = 0x80017ee0
执行一次 si：pc = 0x80200004，sp = 0x80203000
再执行一次 si：停在 tail kern_init
```

当前 ELF 符号表中 bootstack = 0x80201000，bootstacktop = 0x80203000，栈大小为 0x2000（8 KiB）。栈向低地址增长。

GDB 将 0x80203000 显示为 SBI_CONSOLE_PUTCHAR，是因为该数据符号与 bootstacktop 恰好同址。bootstacktop 是栈区域的上界标签，不占存储空间；第一次分配栈空间会向低地址移动，因此该显示不意味着栈初始化错误。

#### tail kern_init

将控制权交给 kern_init，不像普通函数调用那样设置返回到入口代码的返回地址。kern_init 声明为不返回，最终进入循环，因此入口无需等待它返回。

本轮执行 tail 后，GDB 进入 kern_init。继续单步，sp 从 0x80203000 变为 0x80202ff0，表明函数建立了 16 字节的栈帧。源码行号在函数头和 memset 之间跳动，与优化后的指令到源码行的映射有关，不能据此认定程序回退或 memset 已经完成。

### 练习2：使用 GDB 验证启动流程

两个终端均进入 lab1/code，分别执行 make debug 和 make gdb。

#### 复位入口与最初指令

本轮连接后 PC 为 0x1000。执行 `x/10i $pc`，得到的前六条指令如下：

| 地址 | 指令 | 作用 |
|---|---|---|
| 0x1000 | auipc t0,0x0 | 将当前指令地址 0x1000 写入 t0 |
| 0x1004 | addi a2,t0,40 | 设置 a2 = 0x1028，指向传给固件的启动信息 |
| 0x1008 | csrr a0,mhartid | 读取当前 hart ID 到 a0 |
| 0x100c | ld a1,32(t0) | 从 0x1020 读取设备树地址到 a1 |
| 0x1010 | ld t0,24(t0) | 从 0x1018 读取固件入口到 t0 |
| 0x1014 | jr t0 | 跳转至 t0 指定的固件入口 |

这里的 0x1000 是本实验 QEMU virt 平台的复位入口，不是所有 RISC-V 硬件统一规定的地址。0x1018 起包含启动数据；反汇编中显示的 unimp 和 .insn 是数据被当作指令解释的结果，不是启动路径接着执行的指令，也不说明发生了非法指令异常。

#### 单步进入 OpenSBI

```text
(gdb) display/i $pc
(gdb) si 5
=> 0x1014: jr t0
(gdb) info registers pc t0 a0 a1 a2
pc = 0x1014
t0 = 0x80000000
a0 = 0x0
a1 = 0x87000000
a2 = 0x1028
(gdb) si
=> 0x80000000: add s0,a0,zero
(gdb) p/x $pc
$1 = 0x80000000
```

这证明前五条指令准备了启动参数和目标地址，第六条 jr 将控制权交给 0x80000000 的 OpenSBI。本轮还观察到固件前三条指令将 a0、a1、a2 保存到 s0、s1、s2：

```text
0x80000000: add s0,a0,zero
0x80000004: add s1,a1,zero
0x80000008: add s2,a2,zero
0x8000000c: jal 0x800006a0
```

#### 命中内核第一条指令

```text
(gdb) b *kern_entry
Breakpoint 1 at 0x80200000: file kern/init/entry.S, line 7.
(gdb) continue
Breakpoint 1, kern_entry () at kern/init/entry.S:7
7    la sp, bootstacktop
=> 0x80200000 <kern_entry>: auipc sp,0x3
```

由此验证本次启动路径为 `0x1000 → 0x80000000 → 0x80200000`。OpenSBI 内部初始化阶段使用 continue 运行到内核断点，没有逐条单步其全部代码。断点命中时，内核第一条机器指令尚未执行；`auipc sp,0x3` 是 `la sp,bootstacktop` 展开后的第一条指令。

## 五、测试与验证

以下摘录本轮正式调试时保存的终端输出，以及最终 QEMU 运行截图中的文字。省略无关寄存器和重复输出，保留验证所需的命令、地址和结果；不使用此前试跑记录。

### 5.1 复位入口与启动参数

连接 QEMU 后，模拟 CPU 暂停在 0x1000。反汇编并执行前五条指令：

```text
0x0000000000001000 in ?? ()
(gdb) x/10i $pc
=> 0x1000:      auipc   t0,0x0
   0x1004:      addi    a2,t0,40
   0x1008:      csrr    a0,mhartid
   0x100c:      ld      a1,32(t0)
   0x1010:      ld      t0,24(t0)
   0x1014:      jr      t0
   0x1018:      unimp
   0x101a:      .insn   2, 0x8000
   0x101c:      unimp
   0x101e:      unimp
(gdb) display/i $pc
1: x/i $pc
=> 0x1000:      auipc   t0,0x0
(gdb) si 5
0x0000000000001014 in ?? ()
1: x/i $pc
=> 0x1014:      jr      t0
(gdb) info registers pc t0 a0 a1 a2
pc             0x1014   0x1014
t0             0x80000000       2147483648
a0             0x0      0
a1             0x87000000       2264924160
a2             0x1028   4136
```

结果：最初执行的六条指令位于 0x1000～0x1014。执行前五条后，a0 为 hart ID 0，a1 为设备树地址 0x87000000，a2 为启动信息地址 0x1028，t0 为固件入口 0x80000000。0x1018 起的内容是启动数据，显示为 unimp 不代表实际发生异常。

### 5.2 从复位代码进入 OpenSBI

```text
(gdb) si
0x0000000080000000 in ?? ()
1: x/i $pc
=> 0x80000000:  add     s0,a0,zero
(gdb) p/x $pc
$1 = 0x80000000
(gdb) x/10i $pc
=> 0x80000000:  add     s0,a0,zero
   0x80000004:  add     s1,a1,zero
   0x80000008:  add     s2,a2,zero
   0x8000000c:  jal     0x800006a0
   0x80000010:  add     a6,a0,zero
   0x80000014:  add     a0,s0,zero
   0x80000018:  add     a1,s1,zero
   0x8000001c:  add     a2,s2,zero
   0x80000020:  li      a7,-1
   0x80000022:  beq     a6,a7,0x8000002a
```

结果：执行 0x1014 处的 jr t0 后，PC 变为 0x80000000，确认控制权进入 OpenSBI。固件首先将三个启动参数保存到 s0、s1、s2。

### 5.3 固件交接到内核入口

```text
(gdb) b *kern_entry
Breakpoint 1 at 0x80200000: file kern/init/entry.S, line 7.
(gdb) continue
Continuing.

Breakpoint 1, kern_entry ()
    at kern/init/entry.S:7
7           la sp, bootstacktop
1: x/i $pc
=> 0x80200000 <kern_entry>:     auipc   sp,0x3
```

结果：固件继续执行后命中 0x80200000 的内核入口断点，验证了从复位入口到内核第一条指令的启动路径。此时第一条内核指令尚未执行。

### 5.4 内核栈初始化与进入 C 函数

本轮正式调试中，另一次入口单步记录如下。两段记录分别验证启动路径和内核入口操作，不拼接为同一次 GDB 会话。

```text
Breakpoint 1, kern_entry () at kern/init/entry.S:7
7           la sp, bootstacktop
(gdb) i r
```

从该次寄存器输出中摘录：

```text
sp             0x80017ee0       0x80017ee0
pc             0x80200000       0x80200000 <kern_entry>
```

继续单步：

```text
(gdb) si
0x0000000080200004 in kern_entry ()
    at kern/init/entry.S:7
7           la sp, bootstacktop
(gdb) i r
```

此时对应的寄存器输出为：

```text
sp             0x80203000       0x80203000 <SBI_CONSOLE_PUTCHAR>
pc             0x80200004       0x80200004 <kern_entry+4>
```

完成 la 的后续指令，再执行 tail：

```text
(gdb) si
9           tail kern_init
(gdb) x/10x $sp
0x80203000 <SBI_CONSOLE_PUTCHAR>:       0x00000001   0x00000000       0x00000000      0x00000000
0x80203010:     0x00000000      0x00000000      0x00000000    0x00000000
0x80203020:     0x00000000      0x00000000
(gdb) si
kern_init () at kern/init/init.c:8
8           memset(edata, 0, end - edata);
```

继续若干次 si 后，观察到：

```text
0x000000008020001a      6       int kern_init(void) {
(gdb) si
0x000000008020001c      8           memset(edata, 0, end - edata);
(gdb) x/10x $sp
0x80202ff0:     0x00000000      0x00000000       0x00000000      0x00000000
0x80203000 <SBI_CONSOLE_PUTCHAR>:       0x00000001   0x00000000       0x00000000      0x00000000
0x80203010:     0x00000000      0x00000000
```

结果：sp 初始化为 bootstacktop 的地址 0x80203000；tail 将执行流程交给 kern_init；函数建立栈帧后 sp 减少 16 字节，变为 0x80202ff0。SBI_CONSOLE_PUTCHAR 与 bootstacktop 同址，GDB 的符号显示不代表栈地址错误。x/10x $sp 从 sp 开始向高地址读内存，不能把所显示的全部内容都视为已使用的栈空间。

### 5.5 内核启动输出

在 make debug 启动的 QEMU 中，由 GDB 继续运行后，终端截图包含以下输出（从截图摘录）：

```text
OpenSBI v0.9
Firmware Base             : 0x80000000
Runtime SBI Version       : 0.2
Domain0 Next Address      : 0x0000000080200000
Domain0 Next Arg1         : 0x0000000087000000
Domain0 Next Mode         : S-mode
(THU.CST) os is loading ...
```

结果：OpenSBI 报告的下一阶段地址为 0x80200000，目标特权级为 S-mode，随后出现内核的启动文字，说明内核已执行到打印阶段。依据 init.c，打印后进入无限循环，终端不返回 shell 是预期行为。

### 5.6 验证结论

| 验证项目 | 实际结果 | 结论 |
|---|---|---|
| 复位入口 | 初始 PC = 0x1000 | 符合本平台预期 |
| 复位代码交接 | jr t0 后 PC = 0x80000000 | 已进入 OpenSBI |
| 固件交接 | 命中 kern_entry，PC = 0x80200000 | 已到达内核第一条指令 |
| 内核栈初始化 | sp = 0x80203000 | 与 bootstacktop 一致 |
| C 函数入口 | 单步进入 kern_init，随后 sp = 0x80202ff0 | 已进入 C 初始化流程并建立栈帧 |
| 启动打印 | 输出 (THU.CST) os is loading ... | 内核运行结果符合预期 |



## 六、实验总结与收获

通过入口单步观察，可区分内核链接地址、固件交接地址以及运行时寄存器状态；栈初始化使后续 C 函数能够建立栈帧。本实验涉及启动、特权级与固件接口，尚未实现进程调度和完整的虚拟内存管理。

小组个人体会、AI 协作经验和最终分工：待本人补充。
