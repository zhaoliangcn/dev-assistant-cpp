# dev-assistant-cpp

代码库级 AI 编程助手（C++ 版）——零第三方运行时依赖，单二进制交付。

对应 Rust 原版 `dev-assistant-rs` 的 C++ 移植，核心能力：

- **多模型支持**：OpenAI 及兼容服务（商汤 / DeepSeek / Kimi / 智谱等）、Anthropic、Ollama 本地模型，REPL 内 `/model` 列表/编号切换，或 `--model` 启动即切
- **交互式 REPL**：`/status` `/model` `/skills` `/memory` `/help` 等斜杠命令
- **工具调用**：读写文件、目录列表、命令执行等内置工具 + 子代理，带安全策略与操作审批
- **Web 模式**：内置 HTTP + WebSocket 服务，流式推送回复
- **会话与记忆**：会话日志（JSONL）、经验记忆库（dream.ingest）、技能扫描、hooks
- **流式输出**：SSE 增量渲染；推理模型 `reasoning_content` 兜底可见回复

## 构建

要求：CMake ≥ 3.16、C++17 编译器；CURL/OpenSSL 可选（缺失时自动走内置 socket + TLS 路径）。

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/dev-assistant --help
```

### Linux musl 静态发布（可选）

用 zig cc 交叉编译全静态二进制（musl + curl + OpenSSL），产物可直接拷到 x86_64 Linux 运行：

```sh
# 前置：ZIG_EXE 指向 zig 可执行文件；vendored OpenSSL/curl 源码放 third_party/
scripts/build-musl-deps.sh          # 交叉编译依赖 → /tmp/*-musl-install（幂等，FORCE=1 重建）
cmake --preset musl-static          # 或 cmake -B build-musl -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-musl.cmake
cmake --build build-musl -j
```

### 测试

```sh
cmake --build build -j --target da_tests && ./build/da_tests
```

## 用法

```sh
dev-assistant                          # 交互式 REPL
dev-assistant -m <消息>                # 单次执行
dev-assistant --web [--port N]         # Web 模式（默认 127.0.0.1:8080）
dev-assistant init                     # 生成配置模板
```

选项：

| 选项 | 说明 |
|---|---|
| `--project <dir>` | 项目工作目录（默认当前目录；工具/配置/会话日志指向该目录） |
| `--config <path>` | 模型配置文件（默认 `<项目目录>/.dev-assistant-models.toml`） |
| `--model <name>`  | 启动即切换模型（按名称或模型 ID） |

## 配置

`init` 生成最小模板；多模型用 Rust 风格数组表（`[[models]]`），支持 `${VAR}` / `${VAR:-default}` 环境变量展开与 `.env` 加载：

```toml
[[models]]
name = "deepseek"                          # 显示名（/model 切换用）
provider = "deepseek"                      # openai / openai-compatible / anthropic / ollama
api_url = "https://api.deepseek.com/v1"    # 缺 /chat/completions 自动补全
api_key = "${DEEPSEEK_API_KEY}"
model = "deepseek-chat"                    # 模型 ID
temperature = 0.2
max_output_tokens = 8192                   # 单次响应输出上限（不发上下文预算）
```

> ⚠️ 该文件含 API 密钥，已被 `.gitignore` 忽略，请勿提交。

## 目录结构

```
src/
├── agent/       # 会话循环、流水线、压缩、token 计数
├── config/      # TOML 解析、${VAR} 展开、多模型配置
├── llm/         # LLM 客户端（HTTP/TLS/OpenAI 协议）
├── tools/       # 工具注册表 + 内置工具 + 子代理
├── security/    # 安全策略 + 操作审批
├── skills/      # 技能扫描
├── dream/       # 记忆库
├── hooks/       # 钩子（session-start/end 等）
├── web/         # HTTP + WebSocket 服务
└── persist/     # 会话日志（JSONL）
tests/           # 单元测试（轻量自研框架，82 断言）
scripts/         # zig 交叉编译工具链脚本
```

## License

MIT
