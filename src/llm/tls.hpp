#pragma once
// TLS 抽象层（对应设计文档 HTTP 层的 TLS 支持）
// 基于 OpenSSL（可选依赖 DA_HAVE_OPENSSL）；无 OpenSSL 时提供空实现，
// 调用方对 https:// 返回明确错误
#include <sys/types.h>

#include <string>

namespace da {

// 对已连接的 TCP fd 做 TLS 握手，之后用 send/recv 替代裸 socket IO
class TlsSession {
public:
  TlsSession() = default;
  ~TlsSession();
  TlsSession(const TlsSession&) = delete;
  TlsSession& operator=(const TlsSession&) = delete;

  // 返回 true 表示握手完成（此后 send/recv 走 TLS）；false 表示失败
  // host 用于 SNI + 证书主机名校验；timeout_sec 作用于握手
  bool wrap(int fd, const std::string& host, long timeout_sec);

  ssize_t send(const void* buf, size_t len);
  ssize_t recv(void* buf, size_t len);
  void close();

  bool active() const { return ssl_ != nullptr; }

  // 编译期是否支持 TLS（无 OpenSSL 构建为 false）
  static bool available();

private:
  void* ssl_ = nullptr;  // SSL*（避免在头文件暴露 OpenSSL 类型）
};

}  // namespace da
