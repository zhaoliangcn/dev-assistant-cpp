#include "web/web.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include <nlohmann/json.hpp>

#include "web/http_server.hpp"
#include "web/ws.hpp"

#include "agent/agent.hpp"
#include "prompt.hpp"

namespace da {

using json = nlohmann::json;

namespace {

// 嵌入式最小前端（单页；生产可由 CMake bin2c 替换）
const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<title>dev-assistant</title>
<style>
body{font-family:sans-serif;max-width:720px;margin:2rem auto;padding:0 1rem}
#log div{margin:.4rem 0;padding:.4rem .6rem;border-radius:6px;background:#f4f4f4;white-space:pre-wrap}
#log div.user{background:#dceaff}
input{width:70%}button{width:20%}
</style></head><body>
<h3>dev-assistant-cpp</h3>
<div id="log"></div>
<input id="msg" placeholder="输入消息…"><button onclick="send()">发送</button>
<script>
var ws=new WebSocket("ws://"+location.host+"/ws");
var streaming=null; // 当前流式输出 div
ws.onmessage=function(e){
  if(!e.data){streaming=null;return} // 空帧=结束标记，闭合流式段
  if(!streaming){ // 新一轮流式输出
    streaming=document.createElement("div");
    streaming.className="assistant";
    streaming.textContent="assistant: ";
    document.getElementById("log").appendChild(streaming);
  }
  streaming.textContent+=e.data; // 逐 token 追加
  var l=document.getElementById("log");l.scrollTop=l.scrollHeight;
};
function who(u,c){var d=document.createElement("div");d.className=u;d.textContent=u+": "+c;
var l=document.getElementById("log");l.appendChild(d);l.scrollTop=l.scrollHeight}
function send(){var i=document.getElementById("msg");if(!i.value)return;
who("user",i.value);ws.send(i.value);i.value=""}
document.getElementById("msg").addEventListener("keydown",function(e){
if(e.key==="Enter")send()});
</script></body></html>)HTML";

struct WebSession {
  // Web 模式：单会话，持有一个 Agent
  std::vector<ChatMessage> history;
};

HttpResponse json_response(json j, int status = 200) {
  HttpResponse r;
  r.status = status;
  r.body = j.dump();
  return r;
}

}  // namespace

int run_web(App& app, int port) {
  HttpServer server;

  // WebSocket 流式对话：握手 + 帧循环 + Agent 增量推送
  server.set_ws_handler([&app](int fd, const HttpRequest& req) {
    // 请求头已由 HttpServer 解析；直接完成握手应答
    std::string key = req.header("sec-websocket-key");
    if (key.empty()) { ::close(fd); return; }

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + ws_accept_key(key) + "\r\n\r\n";
    if (::send(fd, resp.data(), resp.size(), 0) <= 0) { ::close(fd); return; }

    // 帧循环：收到文本帧 → 作为用户消息跑一轮 Agent，增量回推
    std::string inbuf;
    char rbuf[4096];
    while (true) {
      ssize_t n = ::recv(fd, rbuf, sizeof rbuf, 0);
      if (n <= 0) break;
      inbuf.append(rbuf, n);
      int opcode;
      std::string payload;
      size_t consumed = 0;
      while (ws_decode_frame(inbuf, opcode, payload, consumed)) {
        inbuf.erase(0, consumed);
        if (opcode == 0x8) {  // close
          ::send(fd, ws_encode_close().data(), 2, 0);
          ::close(fd);
          return;
        }
        if (opcode == 0x9) {  // ping → pong
          std::string pong = "\x8A";
          pong += (char)payload.size();
          pong += payload;
          ::send(fd, pong.data(), pong.size(), 0);
          continue;
        }
        if (opcode != 0x1 || payload.empty()) continue;

        // 一轮对话：独立 Agent，逐 token 增量经 WS 推送
        Agent agent(app.llm(), app.tools(), app.security(), app.approval());
        agent.run(payload, /*interactive=*/false,
                  [fd](const std::string& delta) {
                    // 并发安全：同一 fd 只有本线程在写
                    std::string frame = ws_encode_text(delta);
                    ::send(fd, frame.data(), frame.size(), 0);
                  });
        // 结束标记：空文本帧让前端闭合流式输出
        std::string done = ws_encode_text("");
        ::send(fd, done.data(), done.size(), 0);
      }
    }
    ::close(fd);
  });

  // 静态首页
  server.route("GET", "/", [](const HttpRequest&) {
    HttpResponse r;
    r.content_type = "text/html; charset=utf-8";
    r.body = kIndexHtml;
    return r;
  });

  // 健康检查 / 状态
  server.route("GET", "/api/status", [&app](const HttpRequest&) {
    auto& m = app.llm().config().current_model();
    return json_response({{"model", m.model},
                          {"api_url", m.api_url},
                          {"skills", app.skills().skills().size()},
                          {"memories", app.dream().memories().size()}});
  });

  // 消息接口：POST /api/message {message: "..."} → 同步执行一轮并返回全文
  server.route("POST", "/api/message", [&app](const HttpRequest& req) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("message") ||
        !body["message"].is_string())
      return json_response({{"error", "需要 message 字段（字符串）"}}, 400);
    std::string msg = body["message"].get<std::string>();

    // Web 模式：复用 Agent，非交互；结果以 JSON 返回（流式走 /ws）
    ChatRequest creq;
    creq.messages.push_back({"system", build_system_prompt(&app.skills()), "", {}});
    creq.messages.push_back({"user", msg, "", {}});
    LlmResponse resp = app.llm().chat(creq, nullptr);
    return json_response({{"content", resp.content},
                          {"finish_reason", resp.finish_reason}});
  });

  // 记忆列表
  server.route("GET", "/api/memories", [&app](const HttpRequest&) {
    json arr = json::array();
    for (const auto& m : app.dream().memories())
      arr.push_back({{"id", m.id}, {"title", m.title}, {"uses", m.use_count}});
    return json_response(arr);
  });

  // WebSocket 流式对话
  server.route("GET", "/ws", [&app](const HttpRequest& req) {
    // HttpServer 的 handler 模式不适合长连接；
    // 握手与帧循环在此 handler 内直接操作 fd 是不行的——
    // 简化设计：WS 走独立升级路径（见下方 serve 前的特殊处理说明），
    // 此 handler 仅在握手头缺失时返回 400
    (void)app;
    HttpResponse r;
    r.status = 400;
    r.body = "{\"error\":\"websocket upgrade required\"}";
    return r;
  });

  std::printf("Web 服务已启动: http://127.0.0.1:%d\n", port);
  // 仅绑定 127.0.0.1（设计 §6 安全要求）
  server.serve("127.0.0.1", port);
  return 0;
}

}  // namespace da
