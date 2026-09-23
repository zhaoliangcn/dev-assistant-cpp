#pragma once
// 持久化：append-only JSONL 事件日志（对应 Rust persist/）
// 文件权限 0600；记录对话 / 工具调用 / 审计事件
#include <fstream>
#include <mutex>
#include <string>

namespace da {

class Journal {
public:
  ~Journal() { close(); }

  // 打开会话日志（.dev-assistant/sessions/<id>.jsonl，0600）
  bool open(const std::string& path);
  void close();

  // 追加一行 JSON 事件（自动加时间戳）
  void append(const std::string& type, const std::string& json_payload);

  bool is_open() const { return file_.is_open(); }

private:
  std::mutex mu_;
  std::ofstream file_;
};

// 会话日志脱敏：替换 API Key / Bearer / JWT / 私钥块为 ***
std::string redact_secrets(const std::string& text);

}  // namespace da
