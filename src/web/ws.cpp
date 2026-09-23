#include "web/ws.hpp"

#include <cstring>
#include <functional>

// SHA1（纯标准库实现，RFC 3174）
namespace da {

namespace {

std::string sha1(const std::string& msg) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
           h3 = 0x10325476, h4 = 0xC3D2E1F0;
  std::string data = msg;
  uint64_t bitlen = (uint64_t)data.size() * 8;
  data += (char)0x80;
  while (data.size() % 64 != 56) data += (char)0;
  for (int i = 7; i >= 0; i--) data += (char)((bitlen >> (i * 8)) & 0xFF);

  auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

  for (size_t off = 0; off < data.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = ((uint8_t)data[off + i * 4] << 24) |
             ((uint8_t)data[off + i * 4 + 1] << 16) |
             ((uint8_t)data[off + i * 4 + 2] << 8) |
             (uint8_t)data[off + i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t tmp = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = tmp;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }
  char out[20];
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; i++)
    for (int j = 3; j >= 0; j--) out[i * 4 + (3 - j)] = (char)((hs[i] >> (j * 8)) & 0xFF);
  return std::string(out, 20);
}

const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string& in) {
  std::string out;
  size_t i = 0;
  while (i + 3 <= in.size()) {
    uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
    out += B64[(v >> 18) & 63];
    out += B64[(v >> 12) & 63];
    out += B64[(v >> 6) & 63];
    out += B64[v & 63];
    i += 3;
  }
  if (in.size() - i == 1) {
    uint32_t v = (uint8_t)in[i] << 16;
    out += B64[(v >> 18) & 63];
    out += B64[(v >> 12) & 63];
    out += "==";
  } else if (in.size() - i == 2) {
    uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
    out += B64[(v >> 18) & 63];
    out += B64[(v >> 12) & 63];
    out += B64[(v >> 6) & 63];
    out += "=";
  }
  return out;
}

}  // namespace

std::string ws_accept_key(const std::string& client_key) {
  const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  return base64(sha1(client_key + GUID));
}

bool ws_decode_frame(const std::string& buf, int& opcode, std::string& payload,
                     size_t& consumed) {
  if (buf.size() < 2) return false;  // 数据不足（保留连接）
  opcode = buf[0] & 0x0F;
  bool masked = (buf[1] & 0x80) != 0;
  uint64_t len = buf[1] & 0x7F;
  size_t pos = 2;
  if (len == 126) {
    if (buf.size() < 4) return false;
    len = ((uint8_t)buf[2] << 8) | (uint8_t)buf[3];
    pos = 4;
  } else if (len == 127) {
    if (buf.size() < 10) return false;
    len = 0;
    for (int i = 0; i < 8; i++) len = (len << 8) | (uint8_t)buf[2 + i];
    pos = 10;
  }
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (buf.size() < pos + 4) return false;
    std::memcpy(mask, buf.data() + pos, 4);
    pos += 4;
  }
  if (buf.size() < pos + len) return false;  // 数据不足
  payload.assign(buf, pos, len);
  if (masked)
    for (uint64_t i = 0; i < len; i++) payload[i] ^= mask[i % 4];
  consumed = pos + len;
  return true;
}

std::string ws_encode_text(const std::string& payload) {
  std::string out;
  out += (char)(0x81);  // FIN + text
  size_t len = payload.size();
  if (len < 126) {
    out += (char)len;
  } else if (len < 65536) {
    out += (char)126;
    out += (char)(len >> 8);
    out += (char)(len & 0xFF);
  } else {
    out += (char)127;
    for (int i = 7; i >= 0; i--) out += (char)(((uint64_t)len >> (i * 8)) & 0xFF);
  }
  out += payload;
  return out;
}

std::string ws_encode_close() {
  return std::string("\x88\x00", 2);
}

}  // namespace da
