# hpulogc 全功能评审（2026-09-30）

> 评审对象：main @ 78d1944（tag `v0.6.8`，rd_v0.6 v0.6.8，12 个已合并
> PR 系列全部合入）。方法：逐功能区对照规范（rd_v0.6）、决策记录
> （D-R1..D-R23）、缺陷登记（P-1..P-8）与测试/CI 门禁现状，给出不足
> 与扩展建议。本文是评审快照，不替代 todo.md（执行队列）与
> implementation_notes（实现注记）。

## 一、功能面总览与总体结论

| 功能区 | 现状 | 评价 |
|---|---|---|
| 核心运行时 | 一级 ring（locked/lockfree × SPSC/MPSC/MPSC/MPMC 六组合）、批量提交、溢出策略、信号安全、fork 安全 | **成熟**。24h 压测 WSL 双变体 PASS，云上重跑在途；P-1..P-6 全部闭环 |
| 输出 sink | 9 类内置（console/rollingfile/syslog/null/tcp/udp/unix/fifo/http）+ 自定义 vtable 契约（ABI v1 + reserved[4]）+ per-sink 异步队列 + filter keys | **完整**。网络/IPC/HTTP 三族语义基线统一（§4.10.8/9/10）；P-8（队列内容错位）已修并有内容级回归 |
| 结构化字段 | 五类值 EX 宏 + wire 编码 + json 渲染 + per-sink 白名单 | **完整**（fields_dropped 已兑现，D-R14） |
| 配置 | INI 词法/语义校验、strict/lenient、热加载、chk_conf CLI、conf_eg 实测 | **完整**；JSON/YAML 已评审不做（D-R17） |
| 平台层 | hpu_fs/path/net/tid/tls/signal/watcher 契约，POSIX 全实现 + Win32 编译桩 | **POSIX 成熟；Win32 仅文件系路径可用**（见 R1） |
| 工具/CI | 48-check 矩阵（linux x64/arm64、macOS 全家桶含 universal/TSan、msvc、mingw、vcpkg）、run_matrix 28 组合、bench 夜间门禁、24h 压测 | **门禁体系健全**；两处未生效点见 R7/R8 |
| 文档 | rd_v0.6 规范（唯一规范源）、README、code_structure、decision_log、implementation_notes、perf 报告族 | **与代码同步**（每个功能 PR 均含文档同步） |

**总体结论**：规划内的 P1/P2 功能全部落地且经全矩阵门禁验证，规范-实现-
测试-文档四层同步良好，缺陷登记/修复流程有效（P-1..P-8 无遗留未修）。
主要缺口集中在**平台覆盖（Windows 网络能力）、投递语义上限（无重试/
无 TLS）、契约可扩展性（reserved 槽未消费）**三处，均为已知且有意控制
范围的项；下述 R1–R5 建议在下一个发布周期排期。

## 二、不足与解决方案（按优先级）

### R1（高）网络型/IPC/HTTP sink 在 Windows 全部不可用

- 现状：五类 sink 均为 POSIX-only；`win32_net.c` 仅为 MSVC /W4 编译
  桩（返回 -1），Windows 构建连 `tcp`/`http` 都不可注册。对"跨平台"
  定位是最大的单点覆盖缺口——Windows 服务端场景只剩 console/rollingfile。
- 解决方案：以 Phase 2 的工单模式拆两步——
  1. `hpu_net` Win32 后端真实现（resolve/connect/send/recv/set_timeout
     的 Winsock 映射，桩内已有错误映射骨架可参照）；tcp/udp 先行
     （语义最简），测试复用 loopback 模式（Winsock 版 socket 辅助进
     tests/portability.h）。
  2. unix/fifo 的 Windows 对应物为独立类型 `namedpipe`（§4.10.9 已
     定调不复用类型名）；http 随 1 步自动解锁（hpu_net 原语已齐）。
- 成本：中。CI 需补 win_msvc.yml 的 sink 矩阵与 win_mingw 冒烟。

### R2（高）投递语义上限：无重试、无 TLS，生产 webhook 场景受限

- 现状：全网络族 at-most-once（§4.10.8 明示），http 不跟随重定向、
  5xx/超时即 failed；TLS 仅明文 + 反代指引（D-R21）。对日志这种
  尽力而为场景是正确的默认，但"审计级/付费事件流"场景需要
  at-least-once 选项，明文 HTTP 在出内网场景等于无认证。
- 解决方案：分两层——
  1. **契约 v2（消费 reserved[4]）**：emit_batch 三态返回
     （成功/部分待重试/失败）+ health/probe 回调。sink 私有重试缓冲
     在 v1 已不可行（任务书 §二.7 探明），这是重试语义的唯一正解。
     ABI 演进按 append-only 规则（hpulogc.h:398），自定义 sink 不破坏。
  2. **TLS 可选编译模块**（破例 §6，需独立决策矩阵）：mbedTLS 优先
     （体积/许可友好），`HPULOGC_ENABLE_TLS` 门控，min 预设排除；
     http sink 的 hpu_net 调用点收敛为传输层接口即可插入。
- 成本：1 为中（契约设计 + 各 sink 适配）；2 为中大（CI 矩阵翻倍）。
  两者都待需求信号再启动，规范侧无需预改。

### R3（中）http sink 的 connect 阶段不受 `timeout ms` 约束

- 现状：阻塞 connect 走 OS 默认超时（跨公网可达数十秒），期间
  （async=on）该实例 worker 卡住，队列满后事件按 D-S5 丢弃计
  dropped。规范已明示边界，但"路由器黑洞地址"类配置错误的暴露
  很慢（start fail-fast 只覆盖连接拒绝，不覆盖静默丢包）。
- 解决方案：`hpu_net` 契约再加一个原语
  `hpu_net_stream_open_timeout(addr, len, timeout_ms)`
  （non-blocking connect + poll，POSIX 单文件实现）；
  http/tcp start 与重连路径改用它，`timeout ms` 语义升级为
  "connect + 请求全程"。属 append-only 扩展，不破坏 ABI；
  测试用不可路由地址（10.255.255.1）断言 start 在 timeout 内失败。
- 成本：小（一个原语 + 两处调用点 + 一用例）。建议纳入下一功能 PR
  顺带交付。

### R4（中）shutdown/热加载线程协同强化（#13，D-R15 降级项）

- 现状：满足 §10.4 行为定义，但 shutdown 靠超时兜底 + 热加载与
  队列 worker 的握手是"足够好"而非"严格"（队列内命令不记录、
  无生产者引用计数延迟释放）。
- 解决方案：按原 D-R15 设计执行——队列内命令记录 + 条件变量握手 +
  引用计数延迟释放。**前置条件：云上 24h 压测重跑归档（ETA 10-01
  17:40 CST）之前不动 shutdown 路径**（todo.md 既有约束）。
- 成本：中。回归面大（signal_safe/fork/hotreload/async 四个测试族），
  建议独立 PR + 全门禁。

### R5（低）测试纵深：HTTP 响应解析无模糊测试、二级队列内容断言为点状

- 现状：fuzz 仅覆盖 INI 词法（fuzz_ini_deterministic）；本会话 P-8
  表明队列/解码路径的内容级断言此前缺位（现已补 http 全套 +
  sink_async_content_no_dup，但 rollingfile/udp/tcp 的异步内容断言
  仍只有计数）。http 响应解析（状态行/头边界/CL 解析）是新的
  手写解析面，虽有 12 用例，畸形响应（撕裂状态行、CL 溢出、
  头块超限）仅隐式覆盖。
- 解决方案：
  1. 给 http 响应读取器补畸形响应用例组（截断/垃圾字节/超长头/
     负 CL/伪 CL——server 可编程已支持）。
  2. 中期：响应解析抽成纯函数后进 fuzz（确定性 harness 模式照抄
     fuzz_ini）。
  3. 给 tcp/udp 异步路径各补一条内容回读用例（P-8 教训的制度化）。
- 成本：小（1、3 各半天；2 视抽函数范围）。

### R6（低）观测面：flush 语义二段式不可见

- 现状：`hpulogc_flush` = ring 排空握手（5s 上限，拥塞下可良性
  ERR_IO，P-6 注记）+ 二级队列 flush_wait；调用方无法区分哪段超时。
- 解决方案：暂不动 API（append-only 原则下返回值只能二值）。可选：
  `hpulogc_get_stats` 增加只读诊断字段（ring 在途/各队列在途，
  append-only 允许结构体尾部追加）。待真实用户报告再启动。

## 三、急需扩展的功能（建议排期）

| 优先级 | 项 | 触发条件 | 前置 |
|---|---|---|---|
| P1 | 24h 压测云上重跑收尾 + v0.6.8 发布定版 | ETA 10-01 17:40 CST | 取数 → perf_report_soak §4.3 回填 → D-R6 完成注记 → `pkill -x soak_driver` |
| P1 | bench 夜间基线转录（todo #2） | 立即可做 | baseline_linux.json metrics 为空，从 artifact 转录后门禁即生效 |
| P2 | R3 connect timeout（hpu_net 扩展） | 下一功能 PR 顺带 | 无 |
| P2 | R1.1 Windows 网络运行支持（tcp/udp/http） | Windows 需求信号或 v0.7 规划 | Winsock 测试辅助 |
| P2 | R2.1 契约 v2（三态返回 + probe） | 重试需求信号 | 需求侧先明确语义（幂等键？重试预算？） |
| P3 | R4 shutdown/热加载强化（#13） | 压测归档后 | 压测收尾 |
| P3 | R5 测试纵深三件套 | 随相邻功能 PR | 无 |
| 远期 | R2.2 TLS 可选模块；Win32 namedpipe 独立类型；Conan recipe | 需求信号 | 各自决策矩阵 |

## 四、明确不建议做的（维持既有决策）

- JSON/YAML 配置（D-R17）、双映射环形缓冲（D-R16）：无需求信号，
  已有实现已覆盖收益。
- 全网络族默认重试：与 at-most-once 投递边界冲突，重复投递的
  复杂度应由契约 v2 + 明确需求的场景承担，不进默认语义。
- async=off 的 http：D-R22 已定禁止，秒级阻塞面无合理用例。

## 五、结论

v0.6.8 是一个**可对外发布的基线**：功能面完整、规范一致、门禁健全、
缺陷清零。建议在 24h 压测云上重跑 PASS 归档后正式定版；下一周期的
资源优先投向 R1（Windows 网络）与 R2.1（契约 v2），两者分别打开
平台覆盖与投递语义两座天花板；R3/R5 作为顺带项消化。
