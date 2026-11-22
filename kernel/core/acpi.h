/* kernel/core/acpi.h - DAY70+: 最小 ACPI：只为“关机”（S5 睡眠态）
 *
 * 为什么需要：QEMU 的 PIIX4 把 PM1a 控制寄存器固定在端口 0x604，老代码直接往那里写就能关机；
 * VMware / 真机不是这个端口（VMware 的电源管理控制器在别的 I/O 基址上），写 0x604 没有任何效果，
 * 最后落到 `cli; hlt`——VMware 对“关中断后 HLT”会弹窗 “The CPU has been disabled by the guest
 * operating system”。正确做法是读固件给的 ACPI 表，找到真正的 PM1a_CNT 端口和 S5 的睡眠类型。
 */
#ifndef LUMA_ACPI_H
#define LUMA_ACPI_H
#include <stdint.h>

/* 解析 ACPI 表（RSDP -> RSDT -> FADT -> DSDT 里的 \_S5_ 对象）。成功返回 0，找不到/损坏返回 -1。
 * 只读固件放在低端内存/BIOS 区的表，不修改任何东西；可以重复调用。 */
int  acpi_init(void);

/* 解析出来的值（acpi_init 成功后有效），供 `sysstat`/调试显示 */
uint16_t acpi_pm1a_cnt(void);       /* PM1a 控制寄存器的 I/O 端口 */
uint16_t acpi_pm1b_cnt(void);       /* 0 = 没有 */
uint16_t acpi_slp_typ_a(void);      /* S5 的 SLP_TYPa，已经左移到 bit 12:10 的位置 */

/* 进入 S5：必要时先 ACPI_ENABLE，再把 (SLP_TYP<<10 | SLP_EN) 写入 PM1a/PM1b。成功会直接断电，
 * 不返回；返回（-1）表示没有 ACPI 或固件没理这次写。调用前调用者应当已关中断。 */
int  acpi_poweroff(void);
#endif
