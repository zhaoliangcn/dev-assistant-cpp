#include "dream/dream.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#include "utils/atomic_write.hpp"
#include "utils/frontmatter.hpp"

namespace da {

namespace {

std::string to_lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

// 简易分词：连续字母数字为一个 token（UTF-8 中文按字节序列整段处理）
std::set<std::string> tokenize(const std::string& text) {
  std::set<std::string> out;
  std::string cur;
  for (char c : text) {
    if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80) {
      cur += c;
    } else if (!cur.empty()) {
      out.insert(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.insert(cur);
  return out;
}

long now_sec() { return (long)std::time(nullptr); }

}  // namespace

double memory_similarity(const Memory& a, const Memory& b) {
  auto ta = tokenize(a.title + " " + a.content);
  auto tb = tokenize(b.title + " " + b.content);
  if (ta.empty() || tb.empty()) return 0.0;
  size_t inter = 0;
  for (const auto& w : ta)
    if (tb.count(w)) inter++;
  return (double)inter / (double)std::min(ta.size(), tb.size());
}

std::string DreamStore::memory_path(const std::string& dir,
                                    const std::string& id) {
  return dir + "/" + id + ".md";
}

bool DreamStore::load_all() {
  memories_.clear();
  DIR* d = ::opendir(dir_.c_str());
  if (!d) return false;  // 目录不存在视为空库
  struct dirent* e;
  while ((e = ::readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name.size() < 3 || name.substr(name.size() - 3) != ".md") continue;
    std::ifstream f(dir_ + "/" + name, std::ios::binary);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();

    Frontmatter fm;
    std::string body;
    Memory m;
    m.id = name.substr(0, name.size() - 3);
    if (parse_frontmatter(text, fm, body)) {
      m.title = fm.get_string("title");
      m.created_at = std::atol(fm.get_string("created_at").c_str());
      m.last_used_at = std::atol(fm.get_string("last_used_at").c_str());
      m.use_count = std::atoi(fm.get_string("use_count").c_str());
      m.tags = fm.get_array("tags");
    }
    if (m.title.empty()) m.title = m.id;
    m.content = body;
    memories_.push_back(std::move(m));
  }
  ::closedir(d);
  return true;
}

bool DreamStore::write_memory(const Memory& m) {
  std::string fm = "---\ntitle: " + m.title + "\ncreated_at: " +
                   std::to_string(m.created_at) + "\nlast_used_at: " +
                   std::to_string(m.last_used_at) + "\nuse_count: " +
                   std::to_string(m.use_count) + "\n";
  if (!m.tags.empty()) {
    fm += "tags:\n";
    for (const auto& t : m.tags) fm += "  - " + t + "\n";
  }
  fm += "---\n";
  auto st = atomic_write(memory_path(dir_, m.id), fm + m.content);
  return st.ok();
}

bool DreamStore::ingest(const std::string& title, const std::string& content,
                        const std::vector<std::string>& tags) {
  Memory m;
  m.id = "mem-" + std::to_string(now_sec()) + "-" +
         std::to_string(memories_.size());
  m.title = title;
  m.content = content;
  m.tags = tags;
  long now = now_sec();
  m.created_at = now;
  m.last_used_at = now;
  m.use_count = 0;
  if (!write_memory(m)) return false;
  memories_.push_back(std::move(m));
  return true;
}

int DreamStore::dedup(double threshold) {
  int removed = 0;
  std::vector<Memory> kept;
  for (auto& m : memories_) {
    bool dup = false;
    for (auto& k : kept) {
      if (memory_similarity(k, m) >= threshold) {
        // 保留更早创建、使用更多的那条；合并 use_count
        k.use_count += m.use_count;
        k.last_used_at = std::max(k.last_used_at, m.last_used_at);
        dup = true;
        removed++;
        break;
      }
    }
    if (!dup) kept.push_back(std::move(m));
  }
  memories_ = std::move(kept);
  return removed;
}

int DreamStore::forget(int max_age_days, int keep_min_uses) {
  long cutoff = now_sec() - (long)max_age_days * 86400;
  std::vector<Memory> kept;
  int removed = 0;
  for (auto& m : memories_) {
    if (m.last_used_at < cutoff && m.use_count <= keep_min_uses) {
      removed++;
    } else {
      kept.push_back(std::move(m));
    }
  }
  memories_ = std::move(kept);
  return removed;
}

void DreamStore::touch(const std::string& id) {
  for (auto& m : memories_) {
    if (m.id == id) {
      m.last_used_at = now_sec();
      m.use_count++;
      return;
    }
  }
}

bool DreamStore::save_all() {
  // 目录同步：删除已不在集合中的旧文件，再重写当前集合
  // （dedup/forget 从 memories_ 移除的条目，其磁盘文件须一并清理）
  std::vector<std::string> live_ids;
  live_ids.reserve(memories_.size());
  for (const auto& m : memories_) live_ids.push_back(m.id);

  DIR* d = ::opendir(dir_.c_str());
  if (d) {
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
      std::string name = e->d_name;
      if (name.size() < 3 || name.substr(name.size() - 3) != ".md") continue;
      std::string id = name.substr(0, name.size() - 3);
      bool live = false;
      for (const auto& lid : live_ids)
        if (lid == id) { live = true; break; }
      if (!live) ::unlink(memory_path(dir_, id).c_str());
    }
    ::closedir(d);
  }
  for (const auto& m : memories_)
    if (!write_memory(m)) return false;
  return true;
}

DreamStore::Report DreamStore::report() const {
  Report r;
  r.total = (int)memories_.size();
  long now = now_sec();
  for (const auto& m : memories_) {
    r.total_uses += m.use_count;
    if (m.created_at > r.newest_at) r.newest_at = m.created_at;
    if (r.oldest_at == 0 || m.created_at < r.oldest_at) r.oldest_at = m.created_at;
    if (m.last_used_at < now - 90L * 86400 && m.use_count == 0) r.stale++;
  }
  return r;
}

}  // namespace da
