#pragma once
// Web 子系统装配（对应 Rust web/）
// HTTP API + WebSocket 流式推送 + 内嵌单页界面
// 默认 127.0.0.1:8080
#include <string>

#include "app.hpp"

namespace da {

// 阻塞运行 Web 模式（serve 循环内处理 WS 升级与推送）
int run_web(App& app, int port);

}  // namespace da
