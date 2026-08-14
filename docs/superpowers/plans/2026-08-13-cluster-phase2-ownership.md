# 期2 实施计划：归属自判 + 307 重定向 + epoch 门控

> **For agentic workers:** 按 TDD（RED→GREEN→回归）逐任务执行，每任务一提交系列内节点，期末评审后进入期3。

**Goal:** 服务端实现 spec §3.6 归属决策矩阵：路由键计算、`is_owner` 自判、非 owner 307 重定向、epoch 门控 bypass、Fallback 就地处理（I0 失效照做）。

**前提:** 期0（HashRing + 黄金向量 + /api/v1/cluster）、期1（RowCache + CachingDatabase + kill-switch）已完成。单节点模式（无 cluster 段）行为必须逐字节不变。

---

## 关键决策（规范性）

| # | 决策 | 理由 |
|---|---|---|
| D1 | **路由哈希输入的 datasource = 客户端可见名**：请求显式给了就用显式值，否则**空串**；绝不用服务端解析后的实际源名 | spec §3.3 现文写"服务端用实际值"是自相矛盾的：LP("main")≠LP("")，两端哈希输入不同即可能选出不同 owner。客户端可见口径是唯一可证明两端同键的编码。**需回写 spec §3.3 修订**；缓存键（RowCacheKey）仍用实际解析值（节点内命名空间，与路由编码解耦） |
| D2 | 新组件 `OwnershipRouter`（src/server/ownership_router.h/.cpp）：持 ClusterView 快照 + HashRing（vnodes=100，attempt=0），提供 `decide()` 与计数器 | 决策矩阵集中一处，test_ownership 可脱离 HTTP 直测 |
| D3 | 归属检查在**借连接之后**：borrow → 用 schema 算路由键 → decide → 非 owner 立即归还连接回 307 | insert 的路由键需要 schema（第一个有值唯一列）；schema 挂在 Database 实例上。正确性优先于 307 路径的一次本地借还 |
| D4 | `Database::set_cache_bypass(bool)` 虚方法默认 no-op，CachingDatabase override：bypass 时 select 直通内层（无命中无回填），**写路径失效不变**（I0） | 借出的连接被单个请求线程独占，per-实例标志安全；不引入 thread_local 黑魔法 |
| D5 | epoch 头**缺失不比对**（curl 兼容，矩阵 "—" 列）；不匹配 ⇒ bypass 读 + 响应回 `X-CppPools-Epoch: <self>` | 提示单调性：头只能更保守 |
| D6 | insert 路由键 = **schema 列序第一个"给了值且非 NULL"的唯一列**；一个都没有（纯自增）⇒ 本地处理、不做归属检查 | 键落库前未知无法路由；insert 不缓存行，任意节点执行仅失效本地键，异节点残留 TTL 封顶（§4.3.2 已声明自增 insert 失效本就是 no-op） |
| D7 | where 列为 REAL 型 ⇒ 本地处理（不参与环路由，spec 决策⑫） | 浮点键不稳定 |
| D8 | 307 响应：`status_text="Temporary Redirect"`、`Location: http://<owner_host>:<owner_port><原 path>`、JSON body 说明；307 不计 error_requests | 307 保留 POST+body；owner 不在 nodes 表（配置错位）⇒ 退化为就地处理不缓存（更保守） |
| D9 | 指标：`cpppools_redirects_total` + `cpppools_cache_bypass_total{reason="not_owner\|fallback\|epoch_mismatch"}`（server 端计数，计数点=决策点） | spec §8.1；not_owner/fallback 分开（运维关键信号） |
| D10 | http_util 新增 `find_header(request_view, name)`（大小写不敏感）；CrudApiContext 扩 `fallback`/`epoch` 两 bool/string + `const OwnershipRouter*` | 现有解析只取请求行，头是新增能力 |
| D11 | 单节点退化：nodes 空 ⇒ 环只有一个 "single" 节点 ⇒ 恒 owner；无 epoch 头不比对 ⇒ 行为与期1 完全一致 | 回归全绿即证明 |

## 决策矩阵 → 代码语义（spec §3.6 五行）

```
Decision decide(key_optional, fallback_header, epoch_header):
  key 缺失（D6/D7 本地情形）          → local, cache 允许（沿用期1语义）
  owner = ring.owner(query_key(key, attempt=0))
  owner == self:
      epoch 头存在 && != self.epoch   → local, cache bypass(epoch_mismatch), 回 X-CppPools-Epoch
      否则（含 fallback 头，单调性）  → local, cache 允许
  owner != self:
      无 fallback 头                  → redirect(owner_location)
      有 fallback 头                  → local, cache bypass(not_owner/fallback), 失效照做
```

---

### Task 1: spec 修订 + OwnershipRouter（决策矩阵核心）

**Files:**
- Modify: `docs/superpowers/specs/2026-08-11-cluster-cache-design.md`（§3.3 D1 修订 + §3.6 补 D6）
- Create: `src/server/ownership_router.h/.cpp`、`test/test_ownership.cpp`
- Modify: `CMakeLists.txt`（注册 test_ownership + 源文件）

- [ ] Step 1: spec 两处修订（§3.3 datasource 编码改为客户端可见口径并说明理由；§3.6 补"无键 insert/REAL 键本地处理"）
- [ ] Step 2: **RED** — test_ownership.cpp 路由器单测（不涉 HTTP）：
  - 3 节点视图构造 router；用 §3.2 哈希口径构造若干键，断言 `decide` 五行逐一（owner+无头=允许缓存；owner+fallback=允许；owner+epoch 不符=bypass+回报头；非owner+无fallback=redirect 且 Location 正确；非owner+fallback=就地 bypass）
  - 单节点视图：恒 local、恒允许缓存
  - 计数器：redirects/bypass reasons 随决策增长
- [ ] Step 3: 跑测试确认编译失败/断言失败（RED 前提）
- [ ] Step 4: **GREEN** — 实现 OwnershipRouter：
```cpp
struct OwnershipDecision {
    enum class Action { kLocal, kRedirect } action{Action::kLocal};
    bool cache_allowed{true};           // false = bypass 读命中与回填（失效不受影响，I0）
    bool epoch_mismatch{false};         // true = 响应须回 X-CppPools-Epoch
    std::string location;               // kRedirect 时：http://host:port
};
class OwnershipRouter {
public:
    explicit OwnershipRouter(ClusterView view);      // 构造时建环（vnodes_per_node）
    // key_datasource/key_table/key_column/key_value：路由 4 元组（D1 口径）；
    // has_fallback/epoch_header：客户端头（epoch 空 = 未携带）。
    OwnershipDecision decide(const std::string& key_datasource, const std::string& table,
                             const std::string& column, const db::Value& value,
                             bool has_fallback, const std::string& epoch_header) const;
    // 计数器（atomic，指标导出用）
    struct Stats { uint64_t redirects, bypass_not_owner, bypass_fallback, bypass_epoch; };
    Stats stats() const;
};
```
  - 环空（单节点合成）时 decide 恒 kLocal+允许；epoch 比对只在头非空时进行
  - redirect Location 查 view.nodes 里 owner 的 host/port；查不到 → 退化 local+bypass（D8）
- [ ] Step 5: 接线 CMakeLists（源 + 测试），跑绿
- [ ] Step 6: 提交 `feat(cluster): OwnershipRouter 归属决策矩阵（307/epoch/Fallback 五行）`

### Task 2: HTTP 接线——头解析 + CrudApiContext + Database bypass

**Files:**
- Modify: `src/server/http_util.h/.cpp`（find_header）、`src/server/crud_api.h`（ctx 扩字段）、`src/server/http_server.cpp`（构 router、填 ctx）、`include/cpppools/db/database.h` + `src/db/caching_database.h`（set_cache_bypass）
- Modify: `test/test_http_util.cpp`、`test/test_caching_database.cpp`

- [ ] Step 1: **RED** — http_util 头解析用例（大小写不敏感、缺失返回空、只匹配完整头名）；caching_database bypass 用例（bypass 时两次读都 miss、写后失效仍生效）
- [ ] Step 2: **GREEN** — find_header；`Database::set_cache_bypass(bool){}` 虚默认；CachingDatabase：成员 `bool bypass_{false}`（被独占借用，无需原子），do_select 判 `bypass_` 直通内层；写路径不变
- [ ] Step 3: CrudApiContext 加 `bool fallback{false}; std::string epoch; const OwnershipRouter* router{nullptr};`；http_server 构造期建 router 成员（ClusterView 已在 config_），route_request 里从原始请求解析两头填入 ctx
- [ ] Step 4: 回归全绿 + 提交 `feat(server): 请求头解析与缓存 per-request bypass 通道`

### Task 3: CRUD 归属门控 + 307 + 指标

**Files:**
- Modify: `src/server/crud_api.cpp`（四 handler 统一门控）、`src/server/http_server.cpp`（metrics 导出 + 307 不计错）
- Modify: `test/test_ownership.cpp`（HTTP 级五行 + B5 专项）

- [ ] Step 1: **RED** — test_ownership HTTP 级（Fixture 用 3 节点 ClusterView 起 HttpServer）：
  - 矩阵五行逐行（307 的 Location/status、epoch mismatch 响应头、fallback 就地）
  - **B5 专项**：非 owner 带 Fallback 的 update 执行成功且**失效计数增长**（借 owner 视角：替补写后 owner 读必为新值——双 fixture 同库或断言 invalidations）
  - bypass 期间不得产生新缓存条目（entries 不增）
  - 单节点 fixture 回归：无头/有头行为与期1 一致（201/200，缓存照命中）
- [ ] Step 2: **GREEN** — 四 handler 统一前置：route_and_prepare 后借连接 → 按操作算路由键（select/update/delete 用 where 列；insert 用 D6 规则查 schema）→ D7 REAL 列跳过门控 → `router->decide(...)`：
  - kRedirect → 归还连接（guard 析构）回 307 + Location + JSON body
  - kLocal + !cache_allowed → `db->set_cache_bypass(true)`，操作完复位
  - epoch_mismatch → resp.extra_headers 加 X-CppPools-Epoch
- [ ] Step 3: metrics：`cpppools_redirects_total` + bypass 三个 reason 行（router.stats()）；route_request 对 3xx 不计 error_requests（现在只计 >=400，天然满足——加注释钉住）
- [ ] Step 4: 全量回归 + 提交 `feat(cluster): CRUD 归属门控——307 重定向、epoch 门控、Fallback 就地处理`

### Task 4: 双节点 e2e + 期末评审

**Files:**
- Create: `scripts/cluster_e2e.sh`（两进程、两临时库、cluster 段互指）

- [ ] Step 1: e2e 脚本：node-a(18201)/node-b(18202) 各挂同一 epoch 的两节点视图（数据源名同为 main、各自独立库——**注意**：同名表跨节点是环路由的前提，两库 schema 相同）；断言：
  - 对多个键 insert/select：非 owner 节点回 307 且 Location 指向 owner（curl -s -o /dev/null -w '%{http_code} %{redirect_url}'）
  - curl -L 跟随后读写成功；owner 节点 metrics 有 hits 增长
  - 带 X-CppPools-Fallback 打非 owner：200/201 就地处理，且该节点 cache_entries 不增
  - 错误 epoch 头打 owner：正常响应 + X-CppPools-Epoch 回显 + entries 不增
- [ ] Step 2: 全量回归 + e2e 通过
- [ ] Step 3: 派 code-reviewer 评审期2提交系列 → 修正 → 复验 → 提交

---

## 验收清单（spec §11 期2）

- [ ] test_hash_ring 对黄金向量（已有，回归不破坏）
- [ ] test_ownership：决策矩阵五行 + B5 专项
- [ ] 本机双节点 e2e（scripts/cluster_e2e.sh）
- [ ] 单节点模式行为不变（既有 17 项测试全绿）
- [ ] 编译零告警

## 自查备忘

- 307 必须保留 POST 方法与 body（选 307 而非 302/303 的原因写进注释）
- decide 的哈希输入必须与 test_hash_ring 的 query_key 完全同口径（attempt=0）
- epoch 比对用字符串相等（头是字符串）；self.epoch 转字符串
- bypass 复位必须在归还连接前（RAII 或显式），防止污染下个请求
