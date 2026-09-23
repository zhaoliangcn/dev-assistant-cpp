#include "llm/http.hpp"

#include "llm/tls.hpp"

#ifdef DA_HAVE_CURL
#include <curl/curl.h>
#else
// 内置轻量客户端（设计文档 P0：无 libcurl 依赖路径）
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace da {

namespace {

#ifdef DA_HAVE_CURL
struct SinkCtx {
  std::string buf;
  std::string preview;  // 响应体预览（前 300 字符，供非 2xx 报错）
  const std::function<void(const std::string&)>* on_line;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<SinkCtx*>(userdata);
  size_t total = size * nmemb;
  ctx->buf.append(ptr, total);
  if (ctx->preview.size() < 300) ctx->preview.append(ptr, std::min(total, 300 - ctx->preview.size()));
  if (!ctx->on_line) return total;
  // 按 SSE 行切分，逐行回调
  size_t pos = 0;
  while (true) {
    size_t nl = ctx->buf.find('\n', pos);
    if (nl == std::string::npos) break;
    std::string line = ctx->buf.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    (*ctx->on_line)(line);
    pos = nl + 1;
  }
  ctx->buf.erase(0, pos);
  return total;
}
#endif

// 把响应体按行切分回调（curl 与 socket 路径共用）
void emit_lines(std::string& buf,
                const std::function<void(const std::string&)>& on_line) {
  if (!on_line) return;
  size_t pos = 0;
  while (true) {
    size_t nl = buf.find('\n', pos);
    if (nl == std::string::npos) break;
    std::string line = buf.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    on_line(line);
    pos = nl + 1;
  }
  buf.erase(0, pos);
}

#ifndef DA_HAVE_CURL

// 解析 http(s)://host[:port]/path 形式 URL；is_https 输出是否 TLS
bool parse_http_url(const std::string& url, std::string& host,
                    std::string& port, std::string& path, bool& is_https) {
  is_https = false;
  size_t rest;
  if (url.rfind("https://", 0) == 0) {
    is_https = true;
    rest = 8;
  } else if (url.rfind("http://", 0) == 0) {
    rest = 7;
  } else {
    return false;
  }
  size_t slash = url.find('/', rest);
  std::string hostport =
      url.substr(rest, slash == std::string::npos ? std::string::npos
                                                  : slash - rest);
  path = slash == std::string::npos ? "/" : url.substr(slash);
  size_t colon = hostport.find(':');
  if (colon != std::string::npos) {
    host = hostport.substr(0, colon);
    port = hostport.substr(colon + 1);
  } else {
    host = hostport;
    port = is_https ? "443" : "80";
  }
  return !host.empty();
}

std::pair<int, std::string> socket_http_post(
    const std::string& url, const std::map<std::string, std::string>& headers,
    const std::string& body, long timeout_sec,
    const std::function<void(const std::string&)>& on_line) {
  std::string host, port, path;
  bool is_https = false;
  if (!parse_http_url(url, host, port, path, is_https))
    return {-1, "非法 URL（仅支持 http:// 与 https://）"};

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* ai = nullptr;
  if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &ai) != 0)
    return {-1, "域名解析失败: " + host};
  int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) {
    ::freeaddrinfo(ai);
    return {-1, "socket 创建失败"};
  }
  struct timeval tv;
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
    ::freeaddrinfo(ai);
    ::close(fd);
    return {-1, "连接失败: " + host + ":" + port};
  }
  ::freeaddrinfo(ai);

  // HTTPS：TCP 连接之上做 TLS 握手（证书校验 + SNI）
  TlsSession tls;
  if (is_https) {
    if (!TlsSession::available())
      return {-1, "TLS 不可用：此构建未编译 OpenSSL，不支持 https://"};
    if (!tls.wrap(fd, host, timeout_sec)) {
      ::close(fd);
      return {-1, "TLS 握手失败: " + host +
                      "（证书校验/网络错误，或服务器不支持）"};
    }
  }
  auto do_send = [&](const void* b, size_t n) -> ssize_t {
    return tls.active() ? tls.send(b, n) : ::send(fd, b, n, 0);
  };
  auto do_recv = [&](void* b, size_t n) -> ssize_t {
    return tls.active() ? tls.recv(b, n) : ::recv(fd, b, n, 0);
  };

  std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
  for (const auto& kv : headers) req += kv.first + ": " + kv.second + "\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  req += body;
  size_t sent = 0;
  while (sent < req.size()) {
    ssize_t n = do_send(req.data() + sent, req.size() - sent);
    if (n <= 0) {
      tls.close();
      ::close(fd);
      return {-1, "发送请求失败"};
    }
    sent += n;
  }

  std::string raw;
  char buf[4096];
  while (true) {
    ssize_t n = do_recv(buf, sizeof buf);
    if (n <= 0) break;
    raw.append(buf, n);
  }
  tls.close();
  ::close(fd);

  // 状态行
  size_t he = raw.find("\r\n\r\n");
  if (he == std::string::npos) return {-1, "响应头不完整"};
  std::string head = raw.substr(0, he);
  std::string body_str = raw.substr(he + 4);
  int code = 0;
  if (head.rfind("HTTP/", 0) == 0) {
    size_t sp1 = head.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos
                                          : head.find(' ', sp1 + 1);
    if (sp2 != std::string::npos) code = std::atoi(head.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
  }
  emit_lines(body_str, on_line);
  return {code, ""};
}

#endif

}  // namespace

std::pair<int, std::string> http_post(
    const std::string& url, const std::map<std::string, std::string>& headers,
    const std::string& body, long timeout_sec,
    const std::function<void(const std::string&)>& on_line) {
#ifdef DA_HAVE_CURL
  CURL* curl = curl_easy_init();
  if (!curl) return {-1, "curl 初始化失败"};

  struct curl_slist* hl = nullptr;
  for (const auto& kv : headers)
    hl = curl_slist_append(hl, (kv.first + ": " + kv.second).c_str());

  SinkCtx ctx;
  ctx.on_line = &on_line;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hl);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    return {-1, std::string("curl 错误: ") + curl_easy_strerror(rc)};
  // 尾行冲洗（无换行结尾的最后一段）
  if (!ctx.buf.empty() && on_line) on_line(ctx.buf);
  // 非 2xx：返回响应体预览，供上层显式报错（避免静默失败）
  if (code < 200 || code >= 300) {
    std::string preview = ctx.preview;
    for (auto& c : preview)
      if (c == '\n' || c == '\r') c = ' ';
    return {(int)code, preview};
  }
  return {(int)code, ""};
#else
  return socket_http_post(url, headers, body, timeout_sec, on_line);
#endif
}

}  // namespace da
