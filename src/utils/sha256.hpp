#pragma once
// SHA-256（纯标准库实现，仅用于 KB source_refs 过期检测）
#include <string>

namespace da {

// 返回小写 hex（64 字符）
std::string sha256_hex(const std::string& data);

}  // namespace da
