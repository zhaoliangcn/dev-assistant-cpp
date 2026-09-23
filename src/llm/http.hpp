#pragma once
// HTTP 传输抽象：libcurl（默认）；无 libcurl 时构建禁用 LLM 功能
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace da {

// 返回 {HTTP 状态码, 错误消息}；状态码 -1 表示传输层失败
// on_line：流式模式下每收到一行（SSE 行）回调一次；可为空
std::pair<int, std::string> http_post(
    const std::string& url, const std::map<std::string, std::string>& headers,
    const std::string& body, long timeout_sec,
    const std::function<void(const std::string&)>& on_line);

}  // namespace da
