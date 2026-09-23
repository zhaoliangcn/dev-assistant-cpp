#pragma once
// 记忆系统（对应 Rust dream/）
// ingest：会话结束提取经验写入记忆库；consolidate：巩固合并；
// dedup：关键词重叠去重；forget：过期遗忘
// 记忆以 Markdown + YAML frontmatter 存储（复用 skills 的解析器）
#include <string>
#include <vector>

namespace da {

struct Memory {
  std::string id;
  std::string title;
  std::string content;
  std::vector<std::string> tags;
  long created_at = 0;   // epoch 秒
  long last_used_at = 0;
  int use_count = 0;
};

class DreamStore {
public:
  explicit DreamStore(std::string dir) : dir_(std::move(dir)) {}

  // 记忆库目录（默认 .dev-assistant/memories/）
  const std::string& dir() const { return dir_; }

  // 扫描加载全部记忆
  bool load_all();

  const std::vector<Memory>& memories() const { return memories_; }

  // ingest：追加一条记忆（写盘）
  bool ingest(const std::string& title, const std::string& content,
              const std::vector<std::string>& tags);

  // dedup：内容相似（关键词重叠率≥阈值）的合并；返回删除条数
  int dedup(double threshold = 0.6);

  // forget：删除 last_used 超过 max_age_days 且 use_count 低的记忆；返回删除条数
  int forget(int max_age_days = 90, int keep_min_uses = 0);

  // consolidate：把重叠度高的相邻记忆重写为单条（简化：dedup + 重排时间戳）
  int consolidate() { return dedup(0.5); }

  // 命中：把某条记忆标记为刚使用
  void touch(const std::string& id);

  // 落盘全部修改（重写目录）
  bool save_all();

private:
  bool write_memory(const Memory& m);
  static std::string memory_path(const std::string& dir, const std::string& id);

  std::string dir_;
  std::vector<Memory> memories_;
};

// 关键词重叠相似度（token 重叠率，0~1）
double memory_similarity(const Memory& a, const Memory& b);

}  // namespace da
