#pragma once
// 最小 HTTP/1.1 服务器（对应 Rust web/ 的 axum 替代）
// socket + accept + 每连接一线程（目标并发 ≤8），路由表支持路径参数
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace da {

struct HttpRequest {
  std::string method;
  std::string path;              // 不含 query
  std::map<std::string, std::string> query;
  std::map<std::string, std::string> headers;  // key 已转小写
  std::string body;

  std::string header(const std::string& k) const {
    auto it = headers.find(k);
    return it == headers.end() ? "" : it->second;
  }
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
  // 注册路由；path 支持 ":param" 段（/api/session/:id）
  void route(const std::string& method, const std::string& path,
             HttpHandler h);

  // 监听并进入 accept 循环（阻塞）。bind_addr 通常为 127.0.0.1
  bool serve(const std::string& bind_addr, int port);

  void stop() { running_ = false; }

  // WebSocket 升级回调：请求带 Upgrade: websocket 时，
  // handle_conn 完成握手请求解析后把 fd 与已解析请求交给回调
  // （回调负责握手应答、帧循环，fd 由回调关闭）
  void set_ws_handler(std::function<void(int fd, const HttpRequest&)> h) {
    ws_handler_ = std::move(h);
  }

private:
  struct RouteEntry {
    std::string method;
    std::vector<std::string> segs;  // 路径段（":x" 为参数段）
    HttpHandler handler;
  };
  bool match_route(const std::string& method, const std::string& path,
                   const RouteEntry** out,
                   std::map<std::string, std::string>* params) const;
  void handle_conn(int fd);

  std::vector<RouteEntry> routes_;
  std::function<void(int fd, const HttpRequest&)> ws_handler_;
  bool running_ = false;
};

}  // namespace da
