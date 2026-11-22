# ISO9660 与 El Torito（Day 57 调研笔记）

目标：Day 58 起 LumaOS 要从光盘（.iso）启动、并能从光盘里读取系统资源。本文把“一个能启动的 ISO 长什么样”讲清楚，
`tools/isoinfo.py` 是按下面流程写的检查器（`tests/day57_test_isoinfo.py` 用独立实现 pycdlib 生成的样本验证它）。

## 1. 光盘的基本单位
* 逻辑扇区 **2048 字节**（硬盘是 512）。LBA 以 2048 为单位，BIOS 读光盘必须用 int 13h 扩展（AH=42h）。
* 前 16 个扇区（LBA 0–15，32 KiB）是 **System Area**：ISO9660 不使用，留给启动代码 / 分区表（混合 ISO 的 MBR 放这里）。

## 2. Volume Descriptor Set（从 LBA 16 开始）
每个描述符正好占一个扇区：

| 偏移 | 长度 | 含义 |
|---|---|---|
| 0 | 1 | 类型：0 = Boot Record，1 = Primary，2 = Supplementary（Joliet），3 = Partition，255 = Terminator |
| 1 | 5 | 标识 `CD001` |
| 6 | 1 | 版本（1） |
| 7.. | 2041 | 类型相关数据 |

按顺序读，直到类型 255（Terminator）。Primary Volume Descriptor 里常用字段：
volume id（偏移 40，32 字节）、volume space size（80，**both-endian** 32 位）、logical block size（128，=2048）、
path table 大小/位置、**根目录记录**（156，34 字节，含根目录 LBA 与长度）。
“both-endian” = 同一个数先写一遍小端、再写一遍大端，读的时候任取其一（isoinfo.py 会校验两份相等）。

## 3. El Torito：让 BIOS 找到启动镜像
1. 描述符集里有一个 **Boot Record**（类型 0），偏移 7 处 32 字节 = `EL TORITO SPECIFICATION`（NUL 补齐）。
2. 偏移 **0x47** 的 4 字节小端整数 = **Boot Catalog 所在 LBA**。
3. **Boot Catalog**（一个扇区，每项 32 字节）：
   * **Validation Entry**：header ID = 1，platform ID（0 = x86，0xEF = EFI），ID 字符串，
     偏移 30–31 = `55 AA`，并且整个 32 字节按 16 位字相加，**和必须为 0**（偏移 28–29 是补偿值）。
   * **Initial/Default Entry**：
     * 偏移 0：`0x88` 可启动 / `0x00` 不可启动
     * 偏移 1：启动介质类型（低 4 位）：0 = **不模拟（No-Emulation）**，1/2/3 = 1.2/1.44/2.88 MB 软盘，4 = 硬盘
     * 偏移 2：装载段地址（0 表示 0x07C0，即物理 0x7C00）
     * 偏移 4：system type（硬盘模拟时 = 分区类型）
     * 偏移 6：**扇区数**（按 512 字节计！No-Emulation 下表示 BIOS 要读入内存的 512 字节扇区数）
     * 偏移 8：**Load RBA**（启动镜像在光盘上的起始 LBA，2048 字节单位）
   * 之后可有 Section Header（0x90 / 最后一个 0x91）+ 若干 Section Entry（多平台/多镜像）。

## 4. 三种启动模式对比
| 模式 | BIOS 做什么 | 对 LumaOS 的意义 |
|---|---|---|
| 软盘模拟 | 把镜像当 A: 盘，int 13h 映射到它 | 只有 1.44 MB，放不下内核 |
| 硬盘模拟 | 镜像当 C:，要有分区表 | 受镜像大小/分区限制 |
| **No-Emulation** | 只把 Load RBA 起的 N 个扇区读到 `段:0000` 并跳转，`DL` = 光盘的 BIOS 驱动号 | **Day 59 采用**：我们的 loader 自己用 int 13h 扩展读 2048 字节扇区，读出内核 |

No-Emulation 的进入条件：CS:IP = 装载段:0，`DL` 为启动设备号；此时没有任何文件系统被 BIOS 理解——
loader 必须自带“读 LBA（2048 B）”的能力。

## 5. 对后续各天的约束
* Day 58 `tools/mkiso.py`：System Area（LBA 0–15）、PVD（16）、Boot Record（17）、Terminator（18）、Boot Catalog（19）、
  启动镜像（20…）、再是目录/文件；volume space size 必须覆盖整个文件。
* Day 59：boot image = loader（扇区数字段按 512 字节计，别按 2048 算错 4 倍）。
* Day 63：从光盘读资源 = 解析 PVD 的根目录记录 + 目录记录链（每条记录 `len, ext_len, LBA(both), size(both), flags, name_len, name`）。
* 兼容性（Day 69）：部分老 BIOS 不支持 int 13h 扩展，需要回退（CHS / 或 No-Emulation 下的 AH=02h）。

## 6. 动手验证
```
pip install pycdlib
python3 tests/day57_test_isoinfo.py        # 解析器 vs 独立实现
python3 tools/isoinfo.py some.iso          # 人类可读 dump
```
