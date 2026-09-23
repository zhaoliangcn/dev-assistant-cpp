cmake_minimum_required(VERSION 3.13)
project(dev-assistant-cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Os)

find_package(CURL)      # 可选：缺失则 NO_CURL，走内置客户端
find_package(OpenSSL)   # 可选

set(DA_SOURCES
  src/main.cpp
  src/app.cpp
  src/llm/client.cpp
  src/llm/http.cpp
  src/llm/tls.cpp
  src/llm/provider/openai.cpp
  src/llm/provider/anthropic.cpp
  src/llm/provider/ollama.cpp
  src/tools/registry.cpp
  src/tools/file_tools.cpp
  src/tools/system_tools.cpp
  src/tools/subagent.cpp
  src/security/policy.cpp
  src/security/approval.cpp
  src/persist/journal.cpp
  src/agent/token_counter.cpp
  src/agent/identity.cpp
  src/agent/compressor.cpp
  src/agent/pipeline.cpp
  src/agent/agent.cpp
  src/orchestrator/orchestrator.cpp
  src/scheduler/scheduler.cpp
  src/dream/dream.cpp
  src/skills/skills.cpp
  src/hooks/hooks.cpp
  src/web/http_server.cpp
  src/web/ws.cpp
  src/web/web.cpp
  src/repl.cpp
  src/prompt.cpp
  src/config/config.cpp
  src/config/toml.cpp
  src/utils/frontmatter.cpp
  src/utils/gitignore.cpp
)

add_executable(dev-assistant ${DA_SOURCES})
target_include_directories(dev-assistant PRIVATE third_party src)

if(CURL_FOUND)
  target_compile_definitions(dev-assistant PRIVATE DA_HAVE_CURL)
  target_link_libraries(dev-assistant PRIVATE CURL::libcurl)
endif()
if(OPENSSL_FOUND)
  target_compile_definitions(dev-assistant PRIVATE DA_HAVE_OPENSSL)
  target_link_libraries(dev-assistant PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(UNIX)
  target_link_libraries(dev-assistant PRIVATE pthread)
endif()

# ---- 单测 ----
enable_testing()
add_executable(da_tests
  tests/test_main.cpp
  tests/test_toml.cpp
  tests/test_frontmatter.cpp
  tests/test_token_counter.cpp
  tests/test_gitignore.cpp
  tests/test_llm.cpp
  tests/test_tools.cpp
  src/config/toml.cpp
  src/utils/frontmatter.cpp
  src/utils/gitignore.cpp
  src/llm/provider/openai.cpp
  src/llm/provider/anthropic.cpp
  src/llm/provider/ollama.cpp
  src/llm/http.cpp
  src/llm/tls.cpp
  src/tools/registry.cpp
  src/tools/file_tools.cpp
  src/tools/system_tools.cpp
  src/tools/subagent.cpp
  src/security/policy.cpp
)
target_include_directories(da_tests PRIVATE third_party src)
add_test(NAME unit COMMAND da_tests)
