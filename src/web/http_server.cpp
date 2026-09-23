#include "web/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>

namespace da {

namespace {

// 读满直到 \r\n\r\n 的头部，再按 Content-Length 读 body
bool recv_request(int fd, HttpRequest& req) {
  std::string raw;
  char buf[4096];
  size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
    raw.append(buf, n);
    header_end = raw.find("\r\n\r\n");
    if (raw.size() > 1024 * 1024) return false;  // 头部过大
  }
  std::string header = raw.substr(0, header_end);
  std::string rest = raw.substr(header_end + 4);

  // 请求行
  size_t line_end = header.find("\r\n");
  std::string reqline =
      line_end == std::string::npos ? header : header.substr(0, line_end);
  size_t sp1 = reqline.find(' ');
  size_t sp2 = reqline.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
  req.method = reqline.substr(0, sp1);
  std::string target = reqline.substr(sp1 + 1, sp2 - sp1 - 1);

  // path + query
  size_t q = target.find('?');
  req.path = q == std::string::npos ? target : target.substr(0, q);
  if (q != std::string::npos) {
    size_t start = q + 1;
    while (start <= target.size()) {
      size_t amp = target.find('&', start);
      std::string kv = target.substr(
          start, amp == std::string::npos ? std::string::npos : amp - start);
      size_t eq = kv.find('=');
      if (eq != std::string::npos)
        req.query[kv.substr(0, eq)] = kv.substr(eq + 1);
      if (amp == std::string::npos) break;
      start = amp + 1;
    }
  }

  // 头部
  size_t pos = line_end == std::string::npos ? header.size() : line_end + 2;
  while (pos < header.size()) {
    size_t eol = header.find("\r\n", pos);
    std::string line = header.substr(
        pos, eol == std::string::npos ? std::string::npos : eol - pos);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      for (auto& c : key) c = (char)std::tolower((unsigned char)c);
      size_t vs = colon + 1;
      while (vs < line.size() && line[vs] == ' ') vs++;
      req.headers[key] = line.substr(vs);
    }
    if (eol == std::string::npos) break;
    pos = eol + 2;
  }

  // body
  size_t content_len = 0;
  auto cl = req.headers.find("content-length");
  if (cl != req.headers.end()) content_len = (size_t)std::atol(cl->second.c_str());
  req.body = rest;
  while (req.body.size() < content_len) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    req.body.append(buf, n);
  }
  return true;
}

void send_response(int fd, const HttpResponse& r) {
  const char* status_text = r.status == 200 ? "OK"
                            : r.status == 404 ? "Not Found"
                            : r.status == 405 ? "Method Not Allowed"
                            : r.status == 400 ? "Bad Request"
                            : "Error";
  std::string head =
      "HTTP/1.1 " + std::to_string(r.status) + " " + status_text + "\r\n" +
      "Content-Type: " + r.content_type + "\r\n" +
      "Content-Length: " + std::to_string(r.body.size()) + "\r\n" +
      "Connection: close\r\n\r\n";
  std::string out = head + r.body;
  size_t sent = 0;
  while (sent < out.size()) {
    ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
    if (n <= 0) break;
    sent += n;
  }
}

}  // namespace

void HttpServer::route(const std::string& method, const std::string& path,
                       HttpHandler h) {
  RouteEntry e;
  e.method = method;
  size_t start = 0;
  if (!path.empty() && path[0] == '/') start = 1;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    e.segs.push_back(path.substr(
        start, slash == std::string::npos ? std::string::npos : slash - start));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  e.handler = std::move(h);
  routes_.push_back(std::move(e));
}

bool HttpServer::match_route(const std::string& method,
                             const std::string& path, const RouteEntry** out,
                             std::map<std::string, std::string>* params) const {
  std::vector<std::string> segs;
  size_t start = 1;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    segs.push_back(path.substr(
        start, slash == std::string::npos ? std::string::npos : slash - start));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  for (const auto& r : routes_) {
    if (r.method != method || r.segs.size() != segs.size()) continue;
    bool ok = true;
    params->clear();
    for (size_t i = 0; i < segs.size(); i++) {
      if (r.segs[i][0] == ':') {
        (*params)[r.segs[i].substr(1)] = segs[i];
      } else if (r.segs[i] != segs[i]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      *out = &r;
      return true;
    }
  }
  return false;
}

void HttpServer::handle_conn(int fd) {
  HttpRequest req;
  if (recv_request(fd, req)) {
    // WebSocket 升级：交给 ws_handler 长连接处理
    std::string upgrade = req.header("upgrade");
    if (!upgrade.empty() && upgrade == "websocket" && ws_handler_) {
      ws_handler_(fd, req);  // fd 归回调所有（含关闭）
      return;
    }
    const RouteEntry* r = nullptr;
    std::map<std::string, std::string> params;
    HttpResponse resp;
    if (match_route(req.method, req.path, &r, &params)) {
      (void)params;  // 参数通过 req.query 附加传递（简化）
      for (auto& kv : params) req.query[kv.first] = kv.second;
      try {
        resp = r->handler(req);
      } catch (const std::exception& e) {
        resp.status = 500;
        resp.body = std::string("{\"error\":\"") + e.what() + "\"}";
      }
    } else {
      resp.status = 404;
      resp.body = "{\"error\":\"not found\"}";
    }
    send_response(fd, resp);
  }
  ::close(fd);
}

bool HttpServer::serve(const std::string& bind_addr, int port) {
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) return false;
  int one = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
    addr.sin_addr.s_addr = INADDR_LOOPBACK;
  }
  if (::bind(listen_fd, (sockaddr*)&addr, sizeof addr) != 0 ||
      ::listen(listen_fd, 8) != 0) {
    ::close(listen_fd);
    return false;
  }

  running_ = true;
  while (running_) {
    int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    // 每连接一线程（detach；目标并发低）
    std::thread([this, fd] { handle_conn(fd); }).detach();
  }
  ::close(listen_fd);
  return true;
}

}  // namespace da
