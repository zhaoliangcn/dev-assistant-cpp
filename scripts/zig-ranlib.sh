#!/bin/sh
# zig ranlib 包装（Makefile 生成器要求 CMAKE_RANLIB 为单命令路径）
# ZIG_EXE 环境变量可覆盖 zig 路径（默认见 scripts/toolchain-linux-musl-cross.cmake）
ZIG="${ZIG_EXE:-/Users/macmima1234/zig-x86_64-macos-0.17.0-dev.304+9787df942/zig}"
exec "$ZIG" ranlib "$@"
