# Zig musl 交叉编译 toolchain（P7：静态发布构建）
# 用法:
#   cmake --preset musl-static   （或手动指定本文件）
#   cmake --build --preset musl-static -j
# 产物: build-musl/dev-assistant —— 静态链接 musl + curl + OpenSSL，可直接拷到目标 Linux 运行
# 要求:
#   - zig（默认本机路径；可用环境变量 ZIG_EXE 覆盖）
#   - 交叉编译的 OpenSSL + curl 静态库：先跑 scripts/build-musl-deps.sh
#     （一键交叉编译 → /tmp/openssl-musl-install 与 /tmp/curl-musl-install，
#       产物已存在时跳过，FORCE=1 全量重建；工具链见 scripts/toolchain-linux-musl-cross.cmake）
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# zig 路径：默认本机安装路径，可用环境变量 ZIG_EXE 覆盖（与 scripts/build-musl-deps.sh 一致）
if(NOT DEFINED ZIG_EXE)
  set(ZIG_EXE "$ENV{ZIG_EXE}")
endif()
if(NOT ZIG_EXE)
  set(ZIG_EXE "zig")  # 从 PATH 查找
endif()
set(ZIG_TARGET "x86_64-linux-musl")

set(CMAKE_C_COMPILER "${ZIG_EXE}" "cc" "-target" "${ZIG_TARGET}")
set(CMAKE_CXX_COMPILER "${ZIG_EXE}" "c++" "-target" "${ZIG_TARGET}")

# 静态链接 libc/libc++（zig 自带）
set(CMAKE_EXE_LINKER_FLAGS "-static")
set(CMAKE_SHARED_LINKER_FLAGS "-static")

# 交叉编译 OpenSSL（静态 .a）：启用 DA_HAVE_OPENSSL
set(OPENSSL_ROOT_DIR "/tmp/openssl-musl-install")
set(OPENSSL_INCLUDE_DIR "/tmp/openssl-musl-install/include")
set(OPENSSL_CRYPTO_LIBRARY "/tmp/openssl-musl-install/lib64/libcrypto.a")
set(OPENSSL_SSL_LIBRARY "/tmp/openssl-musl-install/lib64/libssl.a")

# 交叉编译 curl（静态 .a，链交叉 OpenSSL）：静态版 HTTP 走 curl 栈
set(CURL_INCLUDE_DIR "/tmp/curl-musl-install/include")
set(CURL_LIBRARY "/tmp/curl-musl-install/lib/libcurl.a")

# 关闭编译器自检（zig 交叉编译不支持 run 自检）
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
