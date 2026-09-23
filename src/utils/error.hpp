#pragma once
// 轻量错误类型，对齐 Rust 版 Error 枚举的常用分类
#include <string>
#include <variant>

namespace da {

enum class Errc {
  Ok = 0,
  Io,          // 文件/网络 IO 失败
  Config,      // 配置缺失或非法
  Http,        // HTTP 请求失败
  Protocol,    // LLM 响应解析失败
  Security,    // 安全策略拒绝
  Tool,        // 工具执行失败
  Cancelled,
  Internal,
};

inline const char* errc_name(Errc e) {
  switch (e) {
    case Errc::Ok: return "ok";
    case Errc::Io: return "io";
    case Errc::Config: return "config";
    case Errc::Http: return "http";
    case Errc::Protocol: return "protocol";
    case Errc::Security: return "security";
    case Errc::Tool: return "tool";
    case Errc::Cancelled: return "cancelled";
    case Errc::Internal: return "internal";
  }
  return "?";
}

// 简化版 expected<T>（C++17 兼容）；错误统一为 {code, message}
template <typename T>
class Result {
public:
  Result(T v) : data_(std::move(v)) {}
  Result(Errc c, std::string msg) : err_(std::make_pair(c, std::move(msg))) {}

  bool ok() const { return !err_; }
  explicit operator bool() const { return ok(); }

  const T& value() const { return data_; }
  T& value() { return data_; }

  Errc code() const { return err_ ? err_->first : Errc::Ok; }
  const std::string& message() const { return err_->second; }

private:
  T data_{};
  std::optional<std::pair<Errc, std::string>> err_;
};

using Status = Result<bool>;  // ok()==true 表示成功

inline Status Ok() { return Status(true); }
inline Status Err(Errc c, std::string msg) { return Status(c, std::move(msg)); }

}  // namespace da
