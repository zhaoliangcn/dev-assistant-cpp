#pragma once
// Token 估算：CJK 字符 ×2、其他 ×0.75（纯查表，零依赖）
#include <string>

namespace da {

inline bool is_cjk_utf8(const std::string& s, size_t i, size_t& adv) {
  unsigned char c = s[i];
  if (c < 0x80) { adv = 1; return false; }
  // UTF-8 多字节，取码点
  unsigned cp = 0; int len = 0;
  if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
  else { adv = 1; return false; }
  for (int k = 1; k < len; k++) {
    if (i + k >= s.size()) { adv = 1; return false; }
    cp = (cp << 6) | (s[i + k] & 0x3F);
  }
  adv = len;
  // CJK 统一表意 + 常用扩展 A + 全角标点
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FA1F);
}

inline size_t estimate_tokens(const std::string& s) {
  size_t cjk = 0, other = 0, i = 0;
  while (i < s.size()) {
    size_t adv = 1;
    if (is_cjk_utf8(s, i, adv)) cjk += 2;
    else other += 1;
    i += adv;
  }
  return cjk + other * 3 / 4;
}

}  // namespace da
