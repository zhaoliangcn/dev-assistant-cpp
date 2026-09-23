#!/bin/sh
# 一键交叉编译 OpenSSL + curl → x86_64-linux-musl 静态库
# 供 cmake/toolchain-linux-musl.cmake（musl-static 预设）使用。
# 产物: ${INSTALL_PREFIX}/openssl-musl-install 与 ${INSTALL_PREFIX}/curl-musl-install
#
# 用法:
#   scripts/build-musl-deps.sh            # 跳过已存在的产物（幂等）
#   FORCE=1 scripts/build-musl-deps.sh    # 全量重建
# 可配置环境变量:
#   ZIG_EXE       zig 可执行文件路径（默认本机路径）
#   OPENSSL_SRC   vendored OpenSSL 源码目录（默认 thirdparty/openssl_3）
#   CURL_SRC      vendored curl 源码目录（默认 thirdparty/curl-8.18.0）
#   INSTALL_PREFIX  安装根目录（默认 /tmp）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ZIG_EXE="${ZIG_EXE:-$(command -v zig || true)}"
: "${ZIG_EXE:?错误: 未找到 zig，请设置 ZIG_EXE 环境变量}"
OPENSSL_SRC="${OPENSSL_SRC:-$SCRIPT_DIR/../third_party/openssl_3}"
CURL_SRC="${CURL_SRC:-$SCRIPT_DIR/../third_party/curl-8.18.0}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/tmp}"

OPENSSL_INSTALL="$INSTALL_PREFIX/openssl-musl-install"
CURL_INSTALL="$INSTALL_PREFIX/curl-musl-install"
CURL_BUILD="$INSTALL_PREFIX/curl-musl-build"
OPENSSL_BUILD="$INSTALL_PREFIX/openssl-musl-src"

if [ ! -x "$ZIG_EXE" ]; then
  echo "错误: 找不到 zig（$ZIG_EXE），请设置 ZIG_EXE" >&2
  exit 1
fi
[ -d "$OPENSSL_SRC" ] || { echo "错误: OpenSSL 源码目录不存在（$OPENSSL_SRC）" >&2; exit 1; }
[ -d "$CURL_SRC" ] || { echo "错误: curl 源码目录不存在（$CURL_SRC）" >&2; exit 1; }

echo "==> zig: $ZIG_EXE"
export ZIG_EXE

# ---------- 1. OpenSSL 3.2.1（静态） ----------
if [ -f "$OPENSSL_INSTALL/lib64/libcrypto.a" ] && [ -z "${FORCE:-}" ]; then
  echo "==> OpenSSL 已存在，跳过（FORCE=1 可重建）: $OPENSSL_INSTALL/lib64/libcrypto.a"
else
  echo "==> 交叉编译 OpenSSL（zig cc -target x86_64-linux-musl）..."
  rm -rf "$OPENSSL_BUILD" "$OPENSSL_INSTALL"
  cp -R "$OPENSSL_SRC" "$OPENSSL_BUILD"
  cd "$OPENSSL_BUILD"
  make clean >/dev/null 2>&1 || true
  CC="$ZIG_EXE cc -target x86_64-linux-musl" \
  AR="$ZIG_EXE ar" \
  RANLIB="$ZIG_EXE ranlib" \
  ./Configure linux-x86_64 -static \
    --prefix="$OPENSSL_INSTALL" no-shared no-tests >/dev/null
  make -j4 >/dev/null
  make install_sw >/dev/null
  echo "==> OpenSSL 完成: $OPENSSL_INSTALL/lib64/libcrypto.a + libssl.a"
fi

# ---------- 2. curl 8.18.0（静态，链交叉 OpenSSL） ----------
if [ -f "$CURL_INSTALL/lib/libcurl.a" ] && [ -z "${FORCE:-}" ]; then
  echo "==> curl 已存在，跳过（FORCE=1 可重建）: $CURL_INSTALL/lib/libcurl.a"
else
  echo "==> 交叉编译 curl（静态，链交叉 OpenSSL）..."
  rm -rf "$CURL_BUILD" "$CURL_INSTALL"
  cmake -S "$CURL_SRC" -B "$CURL_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/toolchain-linux-musl-cross.cmake" \
    -DCMAKE_C_FLAGS="-DHAVE_STRUCT_TIMEVAL=1 -O3" \
    -DCMAKE_USE_OPENSSL=ON -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DOPENSSL_ROOT_DIR="$OPENSSL_INSTALL" \
    -DOPENSSL_INCLUDE_DIR="$OPENSSL_INSTALL/include" \
    -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_INSTALL/lib64/libcrypto.a" \
    -DOPENSSL_SSL_LIBRARY="$OPENSSL_INSTALL/lib64/libssl.a" \
    -DPKG_CONFIG_LIBDIR="$INSTALL_PREFIX/empty-pkgconfig" \
    -DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=OFF \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBIDN2=OFF -DUSE_LIBIDN2=OFF \
    -DCURL_ZLIB=OFF -DCURL_ZSTD=OFF -DCURL_BROTLI=OFF \
    -DCURL_DISABLE_LDAP=ON -DCURL_USE_NGHTTP2=OFF -DUSE_NGHTTP2=OFF \
    -DBUILD_CURL_EXE=OFF -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$CURL_INSTALL" >/dev/null
  mkdir -p "$INSTALL_PREFIX/empty-pkgconfig"
  cmake --build "$CURL_BUILD" -j4 >/dev/null
  cmake --install "$CURL_BUILD" >/dev/null
  echo "==> curl 完成: $CURL_INSTALL/lib/libcurl.a"
fi

echo "==> 全部就绪。"
echo "    构建静态版: cmake --preset musl-static && cmake --build --preset musl-static -j"
