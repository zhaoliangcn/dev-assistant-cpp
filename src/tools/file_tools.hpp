#pragma once
// 文件工具：read_file / write_file / edit_file / glob / grep（对应 Rust tools/）
#include "tools/registry.hpp"

namespace da {

void register_file_tools(ToolRegistry& reg);

}  // namespace da
