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

// Cut to at most `limit` bytes without splitting a UTF-8 sequence. A raw
// substr()/resize() at an arbitrary byte offset can land in the middle of a
// multi-byte character, and the resulting fragment is not merely ugly: it is
// not representable in JSON, so nlohmann's dump() throws type_error.316 even
// in non-strict mode. Always truncate through this helper.
inline std::string truncate_utf8(const std::string& in, std::size_t limit) {
  if (in.size() <= limit) return in;

  // Find the last complete character that ends at or before `limit`.
  std::size_t i = 0;
  std::size_t last_good = 0;  // byte offset just past the last whole character
  while (i < in.size()) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    std::size_t need = 1;
    if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;

    // The whole character must fit within the limit.
    if (i + need > in.size() || i + need > limit) break;

    // Continuation bytes must be well formed.
    bool ok = true;
    for (std::size_t k = 1; k < need; ++k) {
      if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) { ok = false; break; }
    }
    if (!ok) break;

    i += need;
    last_good = i;
  }
  return in.substr(0, last_good);
}


}  // namespace da
