# Prompt 1：生成 Buddy System 分配模块

你是一名熟悉 RISC-V ucore 的内核开发者。请阅读本仓库 lab2 的
kern/mm/pmm.h、pmm.c、memlayout.h、default_pmm.c、best_fit_pmm.c、libs/list.h
及 Makefile，基于现有物理内存管理接口实现 Buddy System。

本阶段仅新增 kern/mm/buddy_pmm.c 和 buddy_pmm.h，导出
buddy_pmm_manager；不要修改 pmm.c 的管理器选择、内核启动代码、已有算法、
共享页结构或 Makefile，不要生成实验报告和验证记录。内核接入留待后续完成。
测试函数由第二份提示词补充，本阶段可以保留一个空的 static buddy_check
回调以保证模块可编译，不能把空回调描述为测试通过。

实现要求：

1. 沿用项目代码风格：返回类型单独一行、四空格缩进、花括号风格与
   default_pmm.c 一致，使用现有 list.h、assert 和 struct Page。
   内部函数声明为 static，不引入动态内存分配、外部依赖或复杂度无关的抽象。
2. 用按阶划分的空闲双向链表管理 2^order 页块，最大阶设置为 31。
   分配 n 页时向上取整到 2 的幂；找到最小可用阶，逐级对半拆分。
   精确满足阶数的块直接分配；不足时返回 NULL，不改变任何状态。
   n=0 是调用约定错误，触发 assert；极大 n 必须安全失败，不能移位溢出。
3. 使用绝对物理页号确定对齐和伙伴：buddy_ppn = ppn XOR (1UL << order)。
   换算回描述符索引前检查下界、上界以及完整块是否落在描述符范围内。
   只合并同阶且完整空闲的伙伴；不能跨越保留页或空洞。
   连续且可用的多个初始化区间允许在满足伙伴关系时合并。
4. 保留 struct Page 布局。空闲首页的 property 保存块页数，并设置
   PG_property；分配首页可使用当前未占用的 flags 位 2 作为模块私有标记，
   property 保存原始请求页数，并由请求推导实际阶数。明确注释该字段约定。
   非首页不持有块首页标记。删除空闲块时清除标记及旧 property。
5. free_pages 必须接收原始分配首页和原始请求页数，只支持整块释放。
   校验分配首页标记、请求大小和范围，拒绝内部页释放、重复释放和大小不匹配。
   非法释放使用 assert，正常释放清理整个实际块并递归或循环合并伙伴。
   nr_free_pages 统计真实可分配页数，例如申请 3 页应扣除 4 页。
6. init_memmap 接受任意合法起点和正页数，将区域拆成对齐的 2 的幂块，
   不遗漏页、不管理范围外的页。支持多次传入互不重叠的区域，验证输入页原先
   为 reserved；保留空洞不变。注意 pmm_manager.init 在 page_init 建立
   pages/npage 之前调用，因此不要在 init 中假设全局描述符数组已经初始化。
7. 为便于隔离测试，核心函数接受一个模块内部的 buddy_area 上下文，包含
   空闲链表、描述符数组、描述符数量、首物理页号和空闲页数。生产回调使用
   一个静态上下文；测试可以使用独立上下文，而不切换当前内核管理器。
8. 实现 pmm_manager 的 init、init_memmap、alloc_pages、free_pages、
   nr_free_pages 回调，头文件提供带 include guard 的管理器声明。
   说明释放约定的必要代码注释应保留。不要复制改写已有算法的测试函数。

完成后检查新增文件能按项目 C 编译选项编译，但不要切换内核管理器或声称
已完成内核启动验证。交付范围仅为上述新增文件。
