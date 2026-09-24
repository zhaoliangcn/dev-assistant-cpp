// LLM 客户端兼容修复的回归测试：URL 补全 / reasoning 兜底 / 非 2xx 报错
#include "test_common.hpp"

#include <string>

#include "llm/client.hpp"
#include "llm/provider/openai.hpp"

using da::apply_stream_delta;
using da::effective_content;
using da::http_error_reason;
using da::normalize_chat_url;
using da::StreamAccum;
using json = nlohmann::json;

// —— api_url 自动补全 /chat/completions ——
static void test_normalize_chat_url() {
  // 完整地址保持不变
  EXPECT_EQ(normalize_chat_url("https://api.x/v1/chat/completions"),
            "https://api.x/v1/chat/completions");
  // 基础地址自动补全
  EXPECT_EQ(normalize_chat_url("https://api.x/v1"),
            "https://api.x/v1/chat/completions");
  EXPECT_EQ(normalize_chat_url("https://api.x/v1/"),
            "https://api.x/v1/chat/completions");
  EXPECT_EQ(normalize_chat_url(""), "chat/completions");
}

// —— 流式 delta 解析 + reasoning 兜底 ——
static void test_stream_content() {
  StreamAccum acc;
  apply_stream_delta({{"content", "你"}, {"role", "assistant"}}, acc, {});
  apply_stream_delta({{"content", "好"}}, acc, {});
  EXPECT_EQ(acc.content, "你好");
  EXPECT(acc.saw_content);
  EXPECT_EQ(effective_content(acc), "你好");
}

static void test_stream_reasoning_fallback() {
  StreamAccum acc;
  // 推理型模型：全程只有 reasoning_content，无正式 content（deepseek-v4-flash 实测）
  apply_stream_delta({{"reasoning_content", "We"}, {"role", "assistant"}}, acc,
                     {});
  apply_stream_delta({{"reasoning_content", "ll"}}, acc, {});
  EXPECT_EQ(acc.content, "");
  EXPECT(!acc.saw_content);
  EXPECT_EQ(acc.reasoning, "Well");
  // 兜底：正式 content 缺席时以推理流作为回复
  EXPECT_EQ(effective_content(acc), "Well");
}

static void test_stream_content_beats_reasoning() {
  StreamAccum acc;
  apply_stream_delta({{"reasoning_content", "思考中"}}, acc, {});
  apply_stream_delta({{"content", "正式回复"}}, acc, {});
  // 出现正式 content 后，后续 reasoning 不再累积
  apply_stream_delta({{"reasoning_content", "更多思考"}}, acc, {});
  EXPECT_EQ(effective_content(acc), "正式回复");
  EXPECT_EQ(acc.reasoning, "思考中");
}

static void test_stream_tool_calls_fragments() {
  StreamAccum acc;
  apply_stream_delta(
      {{"tool_calls",
        {{{"index", 0},
          {"id", "call_1"},
          {"function", {{"name", "read_"}, {"arguments", "{\"path\""}}}}}}},
      acc, {});
  apply_stream_delta(
      {{"tool_calls",
        {{{"index", 0},
          {"function", {{"name", "file"}, {"arguments", ":\"a.txt\"}"}}}}}}},
      acc, {});
  EXPECT_EQ(acc.pending_calls.size(), 1u);
  auto& tc = acc.pending_calls[0];
  EXPECT_EQ(tc.id, "call_1");
  EXPECT_EQ(tc.name, "read_file");
  EXPECT_EQ(tc.arguments, "{\"path\":\"a.txt\"}");
}

// —— 非 2xx 显式报错文案 ——
static void test_http_error_reason() {
  EXPECT_EQ(http_error_reason(400, "{\"error\":\"bad\"}"),
            "http_error: HTTP 400 {\"error\":\"bad\"}");
  EXPECT_EQ(http_error_reason(429, "inference exceeds tpm/rpm limit"),
            "http_error: HTTP 429 inference exceeds tpm/rpm limit");
  EXPECT_EQ(http_error_reason(-1, "curl 错误: timeout"),
            "http_error: curl 错误: timeout");
}

// —— C5：重试判定（4xx 立即失败，传输错误/429/5xx 可重试）——
static void test_retryable_http_error() {
  // 4xx 参数/鉴权错误：不重试
  EXPECT(!da::retryable_http_error("http_error: HTTP 401 bad key"));
  EXPECT(!da::retryable_http_error("http_error: HTTP 400 bad request"));
  EXPECT(!da::retryable_http_error("http_error: HTTP 403 forbidden"));
  EXPECT(!da::retryable_http_error("http_error: HTTP 404 not found"));
  // 429 / 5xx / 传输错误：可重试
  EXPECT(da::retryable_http_error("http_error: HTTP 429 rate limited"));
  EXPECT(da::retryable_http_error("http_error: HTTP 502 bad gateway"));
  EXPECT(da::retryable_http_error("http_error: HTTP 503 unavailable"));
  EXPECT(da::retryable_http_error("http_error: curl 错误: timeout"));
  // 非 http_error 前缀：不重试（正常 finish_reason）
  EXPECT(!da::retryable_http_error("stop"));
  EXPECT(!da::retryable_http_error("tool_calls"));
}

int test_llm() {
  test_normalize_chat_url();
  test_stream_content();
  test_stream_reasoning_fallback();
  test_stream_content_beats_reasoning();
  test_stream_tool_calls_fragments();
  test_http_error_reason();
  test_retryable_http_error();
  return g_stats.failed;
}
