#pragma once
#include <cstddef>
#include <string>

namespace da {

// nlohmann's dump() throws json::type_error.316 on strings that are not valid
// UTF-8. Tools return raw file bytes, so a latin-1 or binary file made dump()
// throw; on the journal path nothing caught it, so the process died with
// std::terminate. Replace every ill-formed sequence with U+FFFD so the value is
// always representable in JSON.
inline std::string sanitize_utf8(const std::string& in) {
  static const char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD
  std::string out;
  out.reserve(in.size());

  const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
  const std::size_t n = in.size();
  std::size_t i = 0;

  while (i < n) {
    const unsigned char c = p[i];

    if (c < 0x80) {  // ASCII fast path
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }

    int need = 0;  // continuation bytes expected after the lead byte
    if ((c & 0xE0) == 0xC0) need = 1;
    else if ((c & 0xF0) == 0xE0) need = 2;
    else if ((c & 0xF8) == 0xF0) need = 3;

    bool ok = (need > 0) && (i + static_cast<std::size_t>(need) < n);
    if (ok) {
      for (int k = 1; k <= need; ++k)
        if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
    }
    // Overlong encodings, UTF-16 surrogates and code points above U+10FFFF are
    // invalid UTF-8 even when the continuation bytes look correct.
    if (ok && need == 1 && c == 0xC0) ok = false;
    if (ok && need == 2 && c == 0xE0 && p[i + 1] < 0xA0) ok = false;
    if (ok && need == 2 && c == 0xED && p[i + 1] >= 0xA0) ok = false;
    if (ok && need == 3 && c == 0xF0 && p[i + 1] < 0x90) ok = false;
    if (ok && need == 3 && c == 0xF4 && p[i + 1] >= 0x90) ok = false;

    if (ok) {
      out.append(in, i, static_cast<std::size_t>(need) + 1);
      i += static_cast<std::size_t>(need) + 1;
    } else {
      out.append(kReplacement, 3);
      ++i;
    }
  }
  return out;
}

}  // namespace da
