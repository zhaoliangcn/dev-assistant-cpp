#include "llm/tls.hpp"

#ifdef DA_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#endif

namespace da {

#ifdef DA_HAVE_OPENSSL

namespace {
// 进程级一次初始化：SSL 库 + 默认证书路径
bool ssl_library_once() {
  static bool ok = [] {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    return true;
  }();
  return ok;
}

// 创建一次共享 CTX（校验服务端证书）
SSL_CTX* ssl_ctx() {
  static SSL_CTX* ctx = [] {
    ssl_library_once();
    SSL_CTX* c = SSL_CTX_new(TLS_client_method());
    if (!c) return c;
    SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(c);
    return c;
  }();
  return ctx;
}
}  // namespace

TlsSession::~TlsSession() { close(); }

bool TlsSession::wrap(int fd, const std::string& host, long timeout_sec) {
  SSL_CTX* ctx = ssl_ctx();
  if (!ctx) return false;
  SSL* ssl = SSL_new(ctx);
  if (!ssl) return false;
  ssl_ = ssl;

  SSL_set_fd(ssl, fd);
  // SNI + 主机名校验（OpenSSL ≥1.1.0）
  SSL_set_tlsext_host_name(ssl, host.c_str());
  SSL_set1_host(ssl, host.c_str());
  SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
  // 读超时走 socket SO_RCVTIMEO；AUTO_RETRY 让 SSL_read 重试非致命错误
  SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

  timeval tv;
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  // 手动握手（阻塞 fd + 超时）
  while (true) {
    int rc = SSL_connect(ssl);
    if (rc == 1) return true;
    int err = SSL_get_error(ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
    close();
    return false;
  }
}

ssize_t TlsSession::send(const void* buf, size_t len) {
  if (!ssl_) return -1;
  return SSL_write(static_cast<SSL*>(ssl_), buf, (int)len);
}

ssize_t TlsSession::recv(void* buf, size_t len) {
  if (!ssl_) return -1;
  return SSL_read(static_cast<SSL*>(ssl_), buf, (int)len);
}

bool TlsSession::available() { return true; }

void TlsSession::close() {
  if (ssl_) {
    SSL_shutdown(static_cast<SSL*>(ssl_));
    SSL_free(static_cast<SSL*>(ssl_));
    ssl_ = nullptr;
  }
}

#else  // 无 OpenSSL：空实现，调用方应拒绝 https://

TlsSession::~TlsSession() = default;

bool TlsSession::wrap(int, const std::string&, long) { return false; }

ssize_t TlsSession::send(const void*, size_t) { return -1; }

ssize_t TlsSession::recv(void*, size_t) { return -1; }

void TlsSession::close() {}

bool TlsSession::available() { return false; }

#endif

}  // namespace da
