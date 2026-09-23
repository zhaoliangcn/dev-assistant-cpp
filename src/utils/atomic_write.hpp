#pragma once
// 原子写：临时文件 + rename（对应 Rust utils/atomic_write）
#include <cstdio>
#include <string>
#include <sys/stat.h>

#include "error.hpp"

namespace da {

inline Status atomic_write(const std::string& path, const std::string& content,
                           mode_t mode = 0600) {
  std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return Err(Errc::Io, "无法打开临时文件: " + tmp);
  size_t n = std::fwrite(content.data(), 1, content.size(), f);
  std::fflush(f);
  ::fchmod(::fileno(f), mode);
  std::fclose(f);
  if (n != content.size()) {
    std::remove(tmp.c_str());
    return Err(Errc::Io, "写入不完整: " + path);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return Err(Errc::Io, "rename 失败: " + path);
  }
  return Ok();
}

}  // namespace da
