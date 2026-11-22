#!/bin/bash
# tools/build_user.sh - DAY71: 编译用户态库 libluma 和 user/programs/*.c，输出 OUT/*.sw
#
#   tools/build_user.sh [OUT]        OUT 默认 user-out
#
# 步骤：crt0.c + libluma.c -> libluma.a；每个 programs/NAME.c -> NAME.o，与 libluma.a 静态链接成
# NAME.sw（ELF32，入口 _start，装载地址 0x08048000——和 hello.sw 一样，见 build.sh 里的说明）。
# -ffreestanding -nostdlib：用户程序不依赖主机的 C 库；-fno-pic/-fno-pie：位置相关的静态可执行文件；
# -fno-stack-protector：没有 canary 支持；-O2 -fno-asynchronous-unwind-tables：小而干净的代码。
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-user-out}"
CC="${CC:-gcc}"; LD="${LD:-ld}"; AR="${AR:-ar}"
CFLAGS=(-m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iuser/include)
rm -rf "$OUT"; mkdir -p "$OUT/obj"
for f in user/lib/*.c; do
    "$CC" "${CFLAGS[@]}" -c "$f" -o "$OUT/obj/$(basename "${f%.c}").o"
done
rm -f "$OUT/libluma.a"
# crt0 不放进 .a（入口文件必须无条件链接）；其余库函数放 .a，没用到的不会进程序
"$AR" rcs "$OUT/libluma.a" $(ls "$OUT"/obj/*.o | grep -v crt0.o)
for p in user/programs/*.c; do
    n="$(basename "${p%.c}")"
    "$CC" "${CFLAGS[@]}" -c "$p" -o "$OUT/obj/prog_$n.o"
    "$LD" -m elf_i386 -static -nostdlib -e _start -Ttext-segment=0x08048000 \
          -z noexecstack -z noseparate-code -z max-page-size=0x1000 --build-id=none -s \
          "$OUT/obj/crt0.o" "$OUT/obj/prog_$n.o" "$OUT/libluma.a" -o "$OUT/$n.sw"
    echo "[user] $n.sw ($(stat -c%s "$OUT/$n.sw") bytes)"
done
