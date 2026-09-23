# curl/OpenSSL 交叉编译 toolchain（zig cc → x86_64-linux-musl）
# 供 scripts/build-musl-deps.sh 复用；用法:
#   cmake -S <curl 源码> -B <build 目录> -DCMAKE_TOOLCHAIN_FILE=<本文件> ...
# ZIG_EXE 环境变量可覆盖 zig 路径；默认值见下
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT DEFINED ZIG_EXE)
  set(ZIG_EXE "$ENV{ZIG_EXE}")
endif()
if(NOT ZIG_EXE)
  set(ZIG_EXE "zig")  # 从 PATH 查找
endif()
set(CMAKE_C_COMPILER "${ZIG_EXE}" "cc" "-target" "x86_64-linux-musl")
# Makefile 生成器要求 ar/ranlib 为单命令路径 → 仓库内包装脚本
set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/zig-ar.sh")
set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.sh")
# 关闭编译器自检（zig 交叉编译不支持 run 自检）
set(CMAKE_C_COMPILER_WORKS 1)
