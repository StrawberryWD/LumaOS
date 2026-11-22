/* kernel/core/acpi.c - DAY70+: 最小 ACPI 关机（见 acpi.h）
 *
 * 查表路径（ACPI 规范 5.x，32 位 RSDT 路径足够：QEMU / VMware / VirtualBox 都提供）：
 *   1. RSDP：16 字节对齐的 "RSD PTR " 签名，只在两个地方找（规范规定）：EBDA 的前 1KB 和
 *      0xE0000~0xFFFFF。EBDA 段地址在 BIOS 数据区 0x40E——但内核页表不映射第 0 页（NULL 保护，
 *      见 paging.h），读不了；所以这里改搜 0x9F000~0x9FFFF（EBDA 通常放在 640KB 顶端）。
 *      校验：前 20 字节字节和 == 0。
 *   2. RSDP.RsdtAddress -> RSDT（签名 "RSDT"，后面是一串 32 位的表指针）-> 找签名 "FACP"（FADT）。
 *   3. FADT：+40 DSDT 地址，+48 SMI_CMD 端口，+52 ACPI_ENABLE 值，+64 PM1a_CNT_BLK，+68 PM1b_CNT_BLK。
 *   4. DSDT 是 AML 字节码。S5 的睡眠类型藏在一个名字叫 "_S5_" 的 Package 里：
 *          "_S5_"  0x12(PackageOp) PkgLength  NumElements  [0x0A] SLP_TYPa  [0x0A] SLP_TYPb ...
 *      这里不实现 AML 解释器，只在 DSDT 字节里找 "_S5_"，按这个固定形状取出两个字节
 *      （PkgLength 的长度由它第一个字节的高 2 位决定；数值 <= 255 前面有 0x0A 前缀，可能没有）。
 *      这是业界（OSDev 教程、各个爱好者内核）通用的“够用就行”的做法。
 *   5. PM1a_CNT 的 bit0 SCI_EN：=1 表示 ACPI 已启用。没启用就先往 SMI_CMD 写 ACPI_ENABLE，等它变 1。
 *   6. 关机：outw(PM1a_CNT, (SLP_TYPa << 10) | (1 << 13))，PM1b 同理（SLP_EN = bit 13）。
 *
 * 所有读固件内存的地方都先检查长度/范围，表损坏时返回 -1，绝不越界。
 */
#define LUMA_KERNEL_SIDE 1
#include "kernel.h"
#include "acpi.h"

static uint16_t g_pm1a, g_pm1b, g_slp_a, g_slp_b;
static uint16_t g_smi_cmd;
static uint8_t  g_acpi_enable;
static int      g_ok;

static int sig_eq(const uint8_t *p, const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int csum_ok(const uint8_t *p, uint32_t n)
{
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s == 0;
}

static const uint8_t *find_rsdp(void)
{
    static const uint32_t ranges[2][2] = { { 0x9F000u, 0xA0000u }, { 0xE0000u, 0x100000u } };
    for (int r = 0; r < 2; r++)
        for (uint32_t a = ranges[r][0]; a < ranges[r][1]; a += 16) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)a;
            if (sig_eq(p, "RSD PTR ", 8) && csum_ok(p, 20))
                return p;
        }
    return 0;
}

/* 表头：签名 4 字节、长度 4 字节(+4)。长度合理才信（最大 1MB，最小 36）。 */
static int table_ok(const uint8_t *t, const char *sig)
{
    if (!sig_eq(t, sig, 4)) return 0;
    uint32_t len = rd32(t + 4);
    if (len < 36 || len > 0x100000u) return 0;
    return csum_ok(t, len);
}

int acpi_init(void)
{
    g_ok = 0; g_pm1a = g_pm1b = g_slp_a = g_slp_b = g_smi_cmd = 0; g_acpi_enable = 0;
    const uint8_t *rsdp = find_rsdp();
    if (!rsdp) return -1;
    const uint8_t *rsdt = (const uint8_t *)(uintptr_t)rd32(rsdp + 16);
    if ((uintptr_t)rsdt < 0x1000u || !table_ok(rsdt, "RSDT")) return -1;
    uint32_t n = (rd32(rsdt + 4) - 36) / 4;
    const uint8_t *fadt = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        const uint8_t *t = (const uint8_t *)(uintptr_t)rd32(rsdt + 36 + i * 4);
        if ((uintptr_t)t >= 0x1000u && sig_eq(t, "FACP", 4) && table_ok(t, "FACP")) { fadt = t; break; }
    }
    if (!fadt) return -1;
    uint32_t flen = rd32(fadt + 4);
    if (flen < 72) return -1;
    const uint8_t *dsdt = (const uint8_t *)(uintptr_t)rd32(fadt + 40);
    g_smi_cmd     = (uint16_t)rd32(fadt + 48);
    g_acpi_enable = fadt[52];
    g_pm1a        = (uint16_t)rd32(fadt + 64);
    g_pm1b        = (uint16_t)rd32(fadt + 68);
    if (g_pm1a == 0 || (uintptr_t)dsdt < 0x1000u || !table_ok(dsdt, "DSDT")) return -1;

    uint32_t dlen = rd32(dsdt + 4);
    for (uint32_t i = 36; i + 8 < dlen; i++) {
        const uint8_t *p = dsdt + i;
        if (!sig_eq(p, "_S5_", 4)) continue;
        /* 前面应当是 NameOp(0x08) 或 "\\"(0x5C)+NameOp；后面第 5 字节是 PackageOp 0x12 */
        if (p[4] != 0x12) continue;
        const uint8_t *q = p + 5;
        q += ((*q & 0xC0) >> 6) + 2;                 /* 跳过 PkgLength（1~4 字节）和 NumElements */
        if (*q == 0x0A) q++;                          /* BytePrefix */
        g_slp_a = (uint16_t)((uint16_t)*q << 10);
        q++;
        if (*q == 0x0A) q++;
        g_slp_b = (uint16_t)((uint16_t)*q << 10);
        g_ok = 1;
        return 0;
    }
    return -1;
}

uint16_t acpi_pm1a_cnt(void)  { return g_pm1a; }
uint16_t acpi_pm1b_cnt(void)  { return g_pm1b; }
uint16_t acpi_slp_typ_a(void) { return g_slp_a; }

int acpi_poweroff(void)
{
    if (!g_ok && acpi_init() != 0)
        return -1;
    if ((inw(g_pm1a) & 1u) == 0 && g_smi_cmd && g_acpi_enable) {      /* SCI_EN = 0：先让固件启用 ACPI */
        outb(g_smi_cmd, g_acpi_enable);
        for (int i = 0; i < 300000 && (inw(g_pm1a) & 1u) == 0; i++)
            asm volatile("pause");
    }
    outw(g_pm1a, (uint16_t)(g_slp_a | (1u << 13)));
    if (g_pm1b)
        outw(g_pm1b, (uint16_t)(g_slp_b | (1u << 13)));
    for (int i = 0; i < 2000000; i++)                                  /* 给虚拟机/主板一点时间断电 */
        asm volatile("pause");
    return -1;
}
