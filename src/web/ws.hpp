#pragma once
// WebSocket（对应设计 §6：握手 SHA1+base64 ~40 行，frame 编解码 ~200 行）
// 文本帧 + ping/pong + close；用于流式输出推送
#include <string>

namespace da {

// 计算 Sec-WebSocket-Accept 值：base64(sha1(key + GUID))
std::string ws_accept_key(const std::string& client_key);

// 解析一个客户端帧；返回 false 表示连接应关闭（协议错误/close）
// opcode: 0x1 文本 0x8 close 0x9 ping 0xA pong
bool ws_decode_frame(const std::string& buf, int& opcode, std::string& payload,
                     size_t& consumed);

// 编码服务端文本帧（不带掩码）
std::string ws_encode_text(const std::string& payload);
// 编码 close 帧
std::string ws_encode_close();

}  // namespace da
