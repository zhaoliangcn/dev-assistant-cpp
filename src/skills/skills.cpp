#include "skills/skills.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace da {

int SkillRegistry::scan(const std::string& skills_dir) {
  skills_.clear();
  DIR* d = ::opendir(skills_dir.c_str());
  if (!d) return 0;
  struct dirent* e;
  while ((e = ::readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    std::string sub = skills_dir + "/" + name;
    struct stat st;
    if (::stat(sub.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

    std::string skill_file = sub + "/SKILL.md";
    std::ifstream f(skill_file, std::ios::binary);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();

    Skill s;
    s.name = name;
    Frontmatter fm;
    std::string body;
    if (parse_frontmatter(text, fm, body)) {
      s.title = fm.get_string("name");
      s.description = fm.get_string("description");
      s.tags = fm.get_array("tags");
      s.disabled = fm.get_string("disabled") == "true";
    }
    if (s.title.empty()) s.title = name;
    s.body = body;
    if (!s.disabled) skills_.push_back(std::move(s));
  }
  ::closedir(d);
  // 按名称排序（注入顺序确定性）
  std::sort(skills_.begin(), skills_.end(),
            [](const Skill& a, const Skill& b) { return a.name < b.name; });
  return (int)skills_.size();
}

const Skill* SkillRegistry::find(const std::string& name) const {
  for (const auto& s : skills_)
    if (s.name == name || s.title == name) return &s;
  return nullptr;
}

std::string SkillRegistry::render_prompt_section() const {
  if (skills_.empty()) return "";
  std::string out = "\n## 可用技能\n"
                    "以下技能可用；当任务匹配其描述时，优先按其指令执行：\n";
  for (const auto& s : skills_) {
    out += "- **" + s.title + "**";
    if (!s.description.empty()) out += ": " + s.description;
    out += "\n";
  }
  return out;
}

std::string SkillRegistry::activate(const std::string& name) const {
  const Skill* s = find(name);
  return s ? s->body : "";
}

}  // namespace da
