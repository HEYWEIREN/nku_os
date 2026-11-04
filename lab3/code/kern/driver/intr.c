#include <intr.h>
#include <riscv.h>
//CSR 是“控制与状态寄存器”，用于控制中断、记录异常
/* intr_enable - enable irq interrupt */
void intr_enable(void) { set_csr(sstatus, SSTATUS_SIE); }

/* intr_disable - disable irq interrupt */
void intr_disable(void) { clear_csr(sstatus, SSTATUS_SIE); }
