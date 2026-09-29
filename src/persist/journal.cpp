#include "persist/journal.hpp"

#include <chrono>
#include <ctime>
#include <regex>
#include <sys/stat.h>
#include <unistd.h>

namespace da {

bool Journal::open(const std::string& path) {
  std::lock_guard<std::mutex> lk(mu_);
  file_.open(path, std::ios::app);
  if (!file_) return false;
  ::chmod(path.c_str(), 0600);
  return true;
}

void Journal::close() {
  std::lock_guard<std::mutex> lk(mu_);
  if (file_.is_open()) file_.close();
}

void Journal::append(const std::string& type, const nlohmann::json& payload) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!file_.is_open()) return;
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  char ts[32];
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
  nlohmann::json entry{{"ts", ts}, {"type", type}};
  if (payload.is_object()) entry.update(payload);  // nlohmann 转义，防畸形 JSONL
  // error_handler=false: never throw on a tool result that holds raw file
  // bytes. Nothing above this point catches exceptions, so a throw here
  // killed the process via std::terminate (reproduced: type_error.316).
  file_ << entry.dump(-1, ' ', false) << "\n";
  file_.flush();
}

std::string redact_secrets(const std::string& text) {
  static const std::regex patterns[] = {
      std::regex(R"((sk-[A-Za-z0-9_\-]{8,}))"),                     // API key
      std::regex(R"((Bearer\s+)[A-Za-z0-9_\-\.]{10,})"),            // Bearer
      std::regex(R"((eyJ[A-Za-z0-9_\-]{5,}\.)[A-Za-z0-9_\-\.]+)"),  // JWT
      std::regex(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----)"),
  };
  std::string out = text;
  for (const auto& re : patterns) out = std::regex_replace(out, re, "***");
  return out;
}

}  // namespace da
