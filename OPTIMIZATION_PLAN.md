# MasterAgent 优化方案

## 概览

基于代码审查，针对 OpenSparX/MasterAgent C++ 端侧 Agent 内核进行性能、内存与可维护性优化。

**基准**: 2.0.0 版本，16/16 测试通过，25个编译警告

---

## 一、Skill Engine 热路径优化

### 问题 1.1：`routeSkills` 冗余深拷贝

**位置**: `src/skill/skill_engine_impl.cpp:318-320`

```cpp
const std::unordered_map<std::string, SkillRecord> snapshot = repository_.snapshot();
```

**问题**:
- `snapshot()` 深拷贝整个 skill map（共享锁下）
- 第二阶段又对每个命中 skill 重新 `repository_.get_record()` 查询（再次加锁）
- 如果 skill 库有 100 个 skill，每次路由都拷贝 100 条记录，实际只用到 top_k 个

**影响**: 每次请求产生 O(N) 内存分配和字符串拷贝，N = skill 总数

**解决方案**:

#### 方案 A（推荐）: 迭代器 + 筛选指针
```cpp
// 第一阶段：只收集指针或 skill_name
std::vector<const SkillRecord*> hit_records;
{
    std::shared_lock lock(repository_.mutex_);
    for (const auto& [_, record] : repository_.skills_by_name_) {
        if (!record.enabled) continue;
        SkillSearchHit hit;
        if (matcher_.match_meta(query, record, hit)) {
            hit_records.push_back(&record);
        }
    }
}
// 第二阶段：只对命中记录加载正文
```

**收益**: 
- 消除 O(N) 深拷贝，改为 O(k) 指针收集，k = 命中数
- 减少锁持有时间（只在匹配时持锁，正文加载在锁外）
- 内存占用从 `sizeof(SkillRecord) * N` 降至 `sizeof(void*) * k`

#### 方案 B（备选）: 分阶段快照
```cpp
std::vector<SkillRecord> candidates = repository_.snapshot_enabled_only();
```
- 增加 `snapshot_enabled_only()` 只拷贝 `enabled=true` 的记录
- 仍需拷贝，但减少无效数据

---

### 问题 1.2：`to_lower_ascii_copy` 重复分配

**位置**: `src/skill/skill_keyword_matcher.cpp:13-18`

```cpp
bool contains_text(const std::string& haystack, const std::string& needle) {
    return to_lower_ascii_copy(haystack).find(to_lower_ascii_copy(needle)) != std::string::npos;
}
```

**问题**:
- 每次 `contains_text` 调用，`haystack` 和 `needle` 各分配一次临时 string
- `match_meta` 中对每个 skill 的每个 `name_zh`/`tag` 都调用此函数
- 如果 query = "播放音乐"，100 个 skill 平均 3 个 name_zh + 2 个 tag = 500 次临时分配

**解决方案**:

```cpp
// 预分配缓冲区，就地小写化
std::string query_lower = to_lower_ascii_copy(query);

bool contains_text_normalized(
    const std::string& haystack,
    const std::string& needle_lower
) {
    std::string haystack_lower;
    haystack_lower.reserve(haystack.size());
    for (char ch : haystack) {
        haystack_lower.push_back(std::tolower(static_cast<unsigned char>(ch)));
    }
    return haystack_lower.find(needle_lower) != std::string::npos;
}
```

**或使用 `string_view` + 就地比较**:
```cpp
bool contains_text_case_insensitive(std::string_view haystack, std::string_view needle);
```

**收益**: 每次匹配减少 50-80% 临时分配

---

### 问题 1.3：正文文件重复读取无缓存

**位置**: `src/skill/skill_content_loader.cpp:10-19`

```cpp
std::optional<std::string> SkillContentLoader::load_body(...) const {
    const std::string text = read_text_file(build_body_file_path(...));
    // ...
}
```

**问题**:
- 每次 `routeSkills` 命中都从磁盘读正文
- 如果 "播放音乐" 每秒被查询 10 次，同一个 `music.play.txt` 被读 10 次
- 无 LRU 缓存，无预加载

**解决方案**:

```cpp
class SkillContentLoader {
    mutable std::unordered_map<std::string, std::string> cache_;
    mutable std::shared_mutex cache_mutex_;

public:
    std::optional<std::string> load_body(...) const {
        {
            std::shared_lock lock(cache_mutex_);
            auto it = cache_.find(record.body_file);
            if (it != cache_.end()) return it->second;
        }
        std::string text = read_text_file(...);
        if (!text.empty()) {
            std::unique_lock lock(cache_mutex_);
            cache_[record.body_file] = text;
        }
        return text;
    }
};
```

**或在 `reloadSkillLibrary` 时预加载全部正文**:
```cpp
// 当前 repository_ 的 SkillRecord 中 context 字段已预留，但未使用
// 改为在 reload 时填充 context，routeSkills 直接返回
```

**收益**: 
- 热点 skill 零磁盘 I/O
- 响应延迟从 ~500µs (SSD读取) 降至 ~5µs (内存拷贝)

---

## 二、编译警告修复

### 问题 2.1：注释中的嵌套 `/*`

**位置**: `include/master_agent/skill/skill_engine.h:9`

**警告**: `warning: '/*' within block comment [-Wcomment]`

**出现次数**: 21次

**修复**: 将 `/* ... */` 风格注释改为 `/** ... */` 或 `//`

---

### 问题 2.2：switch 语句缺失枚举分支

**位置**: `app/main.cpp:70`

**警告**: `enumeration values 'Suspended' and 'Compensating' not handled in switch [-Wswitch]`

**修复**:
```cpp
case TaskExecutionState::Suspended:
case TaskExecutionState::Compensating:
    // Handle or explicitly fall through
    break;
```

---

## 三、内存管理优化

### 问题 3.1：`SkillRecord` 字符串冗余

**位置**: `include/master_agent/skill/skill_engine.h:54-72`

**问题**:
- `name_zh` 和 `tag` 是 `std::vector<std::string>`
- 如果 100 个 skill，每个平均 3 个 name 和 2 个 tag，共 500 个独立 string 对象
- `category_tag` 高度重复（如 "导航"、"娱乐"），但每个 skill 独立存储

**解决方案**:

```cpp
// 使用字符串驻留池
class StringPool {
    std::unordered_set<std::string> pool_;
    std::shared_mutex mutex_;
public:
    const std::string* intern(const std::string& s) {
        std::unique_lock lock(mutex_);
        return &(*pool_.insert(s).first);
    }
};

struct SkillRecord {
    const std::string* category_tag;  // 指向池化字符串
    std::vector<const std::string*> name_zh;
    // ...
};
```

**收益**: 如果 50 个 skill 共享 10 个 category，内存占用从 `50 * avg_len` 降至 `10 * avg_len`

---

## 四、架构改进

### 问题 4.1：`repository_.snapshot()` 语义不清

**位置**: `src/skill/skill_repository.cpp:26-29`

**问题**:
- `snapshot()` 命名暗示"历史快照"或"不可变视图"，但实际只是"深拷贝"
- 调用方误以为快照是廉价操作，实际每次都全量拷贝
- 无 COW（Copy-On-Write）优化

**建议**:
- 重命名为 `get_all_skills_copy()`，明确语义
- 或实现真正的不可变快照（使用 `std::shared_ptr<const unordered_map>`）

---

### 问题 4.2：匹配逻辑耦合在引擎内

**位置**: `src/skill/skill_keyword_matcher.cpp`

**问题**:
- 当前只支持简单子串匹配
- 如需支持正则、拼音、嵌入向量，需重写 `match_meta`
- 但 `SkillKeywordMatcher` 无虚接口，不支持策略模式

**建议**:
```cpp
class ISkillMatcher {
public:
    virtual bool match(const std::string& query, const SkillRecord& record, SkillSearchHit& hit) const = 0;
};

class KeywordMatcher : public ISkillMatcher { /*...*/ };
class EmbeddingMatcher : public ISkillMatcher { /*...*/ };

class SkillEngineImpl {
    std::unique_ptr<ISkillMatcher> matcher_;
};
```

---

## 五、实施优先级

### P0 (立即修复)
1. 编译警告清零（2.1, 2.2）
2. `routeSkills` 冗余拷贝优化（1.1 方案 A）

### P1 (本周内)
3. 字符串小写化优化（1.2）
4. 正文缓存（1.3，启用 `SkillRecord.context` 预加载）

### P2 (下一迭代)
5. 字符串池化（3.1，针对 category_tag）
6. 匹配器接口抽象（4.2）

---

## 六、预期收益

| 优化项 | 延迟改善 | 内存改善 | 实施复杂度 |
|--------|---------|---------|-----------|
| 1.1 snapshot 消除 | -20% | -60% | 低 |
| 1.2 字符串分配优化 | -15% | -10% | 低 |
| 1.3 正文缓存 | -40% | +5% | 中 |
| 2.1/2.2 警告修复 | 0% | 0% | 极低 |
| 3.1 字符串池化 | -5% | -25% | 中 |

**综合预期**: 端到端 `routeSkills` 延迟降低 **50-70%**，内存占用降低 **40-50%**

---

## 七、验证方法

### 性能基准测试
```bash
# 添加微基准
tests/bench_skill_routing.cpp

# 测试场景：
# - 100 个 skill 库
# - 查询 "播放音乐" 1000 次
# - 测量 P50/P99 延迟和内存分配次数
```

### 回归测试
```bash
ctest --test-dir build --output-on-failure
# 确保 test_prompt_skill 仍然通过
```

---

## 八、风险与缓解

| 风险 | 影响 | 缓解措施 |
|-----|------|---------|
| 指针生命周期问题（方案 A） | 崩溃 | 在 `hit_records` 持有锁期间完成匹配，或拷贝 skill_name 而非指针 |
| 缓存一致性（1.3） | 脏读 | `reloadSkillLibrary` 时清空缓存 |
| 字符串池内存泄漏（3.1） | 内存增长 | 设置池大小上限或 LRU 淘汰 |

---

## 参考

- 代码位置: `https://github.com/OpenSparX/MasterAgent`
- 版本: v2.0.0 (commit 1a6379f)
- CI: `.github/workflows/ci.yml`
