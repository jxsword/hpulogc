# hpulogc 决策记录（decision log）

> 本文件记录项目中所有需要用户决策的事项：**全部候选选项及权衡**、最终结论、
> 选择理由与影响面。未被选中的选项连同弃用理由一并保留——目的是留存分析与
> 取舍过程，供学习与回溯，而非只存结论。
>
> 实现层的自由度决策（非用户决策项）另见 `docs/implementation_notes.md` 的
> 决策索引；需求规范见 `docs/rd_v0.6.md`。

---

## D-M1 MPMC 多消费者的协作语义（2026-09-28，Phase 4）

- **背景**：为日志库新增 MPMC（多生产者多消费者）编译期并发模式，多个消费
  线程如何协作直接决定改动面、可验证性与顺序保证。
- **选项**：
  - A. **单共享环 + N 个对称消费者**——每条记录恰好被一个消费者认领处理；
    处理阶段沿用全局 conf_lock，本期并行化「出队/认领」环节。优点：改动最
    小、与单环架构延续性最好、正确性易证（环层认领协议一次定案）；缺点：
    落盘吞吐仍受 conf_lock 限制、记录间全局输出顺序不再保证。
  - B. 单环 + 每消费者独立输出缓冲——真正并行落盘。优点：吞吐扩展性最好；
    缺点：rotation、flush/fsync、热重载句柄迁移、统计口径都要按消费者重构，
    改动量约等于重做输出层。
  - C. 按 category 分片多环——category 哈希分片到 K 个 MPSC 环，每环 1 个
    消费者。优点：类别内严格有序、类别间并行，最符合日志语义；缺点：引入
    K 个环的生命周期/flush/shutdown 聚合，偏离「基于现有架构扩展」最远。
- **结论**：A（用户委托按最佳策略选择）。
- **理由**：与既有单环架构的延续性最高，正确性可被 exactly-once 测试矩阵
  严格验证；顺序保证的变化在文档中明示即可；B/C 的收益在 conf_lock 串行
  处理被消除前无法兑现，列为后续优化路径。
- **影响**：rd_v0.3 §1.1；ring 出队认领协议；`ring_stress_mpmc_*` 测试族；
  全局顺序保证明示。

## D-M2 MPMC 消费者线程数量的来源（2026-09-28，Phase 4）

- **背景**：并发模式是编译期选择，但消费者数量需要一个决定来源。
- **选项**：
  - A. **配置键 `[async] consumer threads`**——默认 1、范围 1–16、init-only、
    热重载忽略；非 MPMC 构建 >1 时按「值合法但构建不支持」启动失败。
    优点：运行时可调、默认值保证配置文件跨构建可移植、与 batch_size 等
    既有 [async] 键风格一致；缺点：所有构建都要解析校验该键。
  - B. 编译期 CMake 选项 `HPULOGC_CONSUMER_THREADS`（默认 2）。优点：与
    「并发模式编译期选择」哲学一致、零配置面改动；缺点：运行时不可调，
    不同部署只能换构建。
- **结论**：A。
- **理由**：消费者数量本质是部署侧资源旋钮（类 batch_size），不是语义开关；
  默认 1 使同一配置文件在 SPSC/MPSC/MPMC 构建间可移植。
- **影响**：rd_v0.3 §1.3；`hpulogc_config_t` 追加 `consumer_threads`；
  fail-fast 清单扩展。

## D-M3 三平台 CI 的 MPMC 覆盖规模（2026-09-28，Phase 4）

- **背景**：MPMC 是新的编译维度，CI 矩阵规模决定验证成本与覆盖面。
- **选项**：
  - A. **全量扩展现有矩阵**——linux-smoke +2 组合（一有锁一无锁）、
    macos-matrix 8→12、msvc-matrix 8→12、TSan 周任务加 MPMC 组合；
    run_matrix.sh 16→24、run_matrix.ps1 8→12。优点：与 SPSC/MPSC 待遇
    完全一致、公共仓库 runner 免费；缺点：CI 时长增加。
  - B. 精简冒烟——每平台仅 1–2 个 MPMC 组合，C 标准维度不展开。优点：
    CI 快；缺点：win/mac 的 MPMC×C11 等组合无 CI 覆盖。
- **结论**：A。
- **理由**：维度组合的编译器/标准差异只能靠矩阵暴露（Phase 4 的 P-3 缺陷
  正是 CI 差异暴露的）；成本可接受。
- **影响**：ci.yml / win_msvc.yml 矩阵；run_matrix 脚本。

## D-M4 MPMC 规范文档的承载方式（2026-09-28，Phase 4）

- **背景**：项目文档已版本化（rd_v0.1/rd_v0.2），MPMC 是规范性变更。
- **选项**：
  - A. **新建 rd_v0.3.md 增量规范**——只写变更项，未触及条款沿用 v0.2；
    同步更新 implementation_notes/code_structure/README。优点：版本快照
    语义干净、变更可审；缺点：规范出现两个文件，需声明优先级。
  - B. 直接修订 rd_v0.2.md 相关章节。优点：规范单点；缺点：破坏 v0.2 的
    版本快照语义。
- **结论**：A。
- **理由**：延续项目版本化文档惯例；（后续 rd_v0.6 合并时 v0.3 内容被
  完整并入，快照仍保留为历史。）
- **影响**：docs/rd_v0.3.md；修订记录。

## D-S1 事件数据模型：结构化字段本期做到什么程度（2026-09-28，Phase 5）

- **背景**：sink 契约草稿引用 `hpulogc_event_t` 与结构化字段统计
  （fields_dropped、HPLOGC_MAX_FIELDS），但未给出定义；事件模型是多 sink
  体系的地基。
- **选项**：
  - A. **最小事件**——event = 现有记录 + 已渲染行视图，fields_dropped
    保留恒 0，结构化字段登记为后续版本。优点：不发 ring 格式、不动渲染
    管线，改动面最小；缺点：网络/聚合型自定义 sink 拿不到结构化数据，
    体系价值打折扣。
  - B. **本期实现结构化字段全量**——事件携带带类型字段数组、预算宏、
    ring 记录格式扩展、渲染层字段支持。优点：sink 体系一步到位，自定义
    sink 可消费结构化数据；缺点：改动面大（ring 双实现、统计恒等式、
    渲染、MPMC 交互全要过一遍）。
- **结论**：B（用户明确选择）。
- **理由**：多 sink 的核心价值在于非文件型 sink（网络/审计/聚合）消费
  结构化数据；分两期做会二次破坏 ring 格式与 ABI。
- **影响**：§4.11 全量；ring 头格式；`hpulogc_log_ex` API；渲染层。

## D-S2 字段值类型系统（2026-09-28，Phase 5）

- **背景**：结构化字段的值如何表达，决定 JSON 保真度与自定义 sink 的
  消费体验。
- **选项**：
  - A. **带类型联合体**（I64/U64/F64/BOOL/STR）——优点：JSON 输出数值
    不带引号、聚合 sink 直接取数值、跨环携带类型字节；缺点：值联合体
    与序列化分支多一些代码。
  - B. 全字符串——优点：实现最简；缺点：JSON 数值语义丢失或需猜类型。
- **结论**：A。
- **理由**：结构化日志的主要消费方是机器（JSON/聚合），类型保真是刚需。
- **影响**：`hpulogc_field_value_t`；ring 字段区 type 字节；json 渲染。

## D-S3 生产端字段 API 形态（2026-09-28，Phase 5）

- **背景**：调用方如何把字段传给日志库。
- **选项**：
  - A. **结构体数组入参** `hpulogc_log_ex(..., const hpulogc_field_t*, size_t, fmt, ...)`——优点：零解析、类型安全、`va_list` 可透传（与异步
    管线兼容）；缺点：调用侧要先构造数组。
  - B. printf 风格变参内嵌键值（fmt 后跟 "k",v,...,NULL 哨兵）——优点：
    调用简短；缺点：需运行时逐参猜类型/约定类型后缀，`va_list` 跨环复制
    不可行（需二次解析）。
- **结论**：A（配 `HPULOGC_*_EX` 便捷宏缓解构造成本）。
- **理由**：与异步管线（va_list 不能跨线程）的技术约束天然兼容。
- **影响**：公共 API；pipeline 生产端。

## D-S4 per-sink 异步投递的实现程度（2026-09-28，Phase 5）

- **背景**：sink 实例通用键 `async` 的语义：现状是「整个管道异步」（单一
  共享环 + 消费者），per-sink 异步若要成立需要额外机件。
- **选项**：
  - A. **键解析但语义延后**——async=on 仅在 ASYNC 构建下等效于默认行为，
    SYNC 构建下启动失败；每 sink 独立队列登记为后续版本。优点：本期改动
    最小；缺点：慢 sink 拖慢全部 sink 的问题不解决。
  - B. **本期实现独立队列 + worker**——每个 async sink 实例一个专属有界
    队列 + 专属 worker 线程，慢 sink 不拖慢其他 sink；代价：flush/shutdown/
    统计/热重载都要按 sink 聚合，新增一套生命周期机件。
- **结论**：B（用户明确选择）。
- **理由**：多 sink 体系的主要动机之一就是异构 sink 速度差异（网络 sink）；
  延后做会使 sink 契约（emit_batch、caps）失去落点，二次改动更贵。
- **影响**：两级队列架构（§4.10.6）；flush/shutdown 聚合；TSan 验证面。

## D-S5 per-sink 异步队列的溢出策略（2026-09-28，Phase 5）

- **背景**：用户先追问了架构事实——「目前全程共用一个日志队列还是某些
  场景有多个？」确认：现状全程只有**一个**共享环（全局溢出策略仅作用于
  它；file sink 内部是字节缓冲不是记录队列）；per-sink 异步才引入第二级
  队列。此追问澄清后重问本项。
- **选项**：
  - A. **恒 discard 不阻塞**——第二级队列满时丢新事件并计入该 sink 的
    dropped；生产者/消费者永不阻塞；与共享环全局策略解耦；需要不丢的
    场景用同步 sink。优点：慢 sink 只丢自己的数据、不影响业务线程；
    缺点：异步投递不保证不丢（文档明示）。
  - B. 跟随全局 overflow policy——wait 会阻塞投递方直到队列有空位（慢
    sink 反压消费者甚至业务线程）。优点：语义统一；缺点：跨层阻塞风险。
  - C. 满时降级同步直写——投递方内联执行该事件的 emit。优点：不丢；
    缺点：调用方可能被慢 sink 拖住，且内联/入队双路径都要线程安全，实现
    最复杂。
- **结论**：A。
- **理由**：与「慢 sink 隔离」的引入动机自洽；阻塞型需求由同步 sink 覆盖；
  实现与验证最简单。
- **影响**：§4.10.6 溢出语义；per-sink dropped 统计；文档明示不保证不丢。

## D-S6 新增 sink API 的命名前缀（2026-09-28，Phase 5）

- **背景**：设计草稿使用 `hplogc_*` 前缀，全库公共 API 是 `hpulogc_*`
  （含 `HPULOGC_API` 导出宏），草稿自身亦有混用。
- **选项**：
  - A. **统一用 hpulogc_**——新 API 全部 `hpulogc_sink_ops_t` /
    `hpulogc_event_t` / `hpulogc_sink_register` / `hpulogc_sink_priv`，
    文档草稿中的 hplogc_ 视为笔误改写。优点：零破坏、与导出宏/ABI 规则
    一致；缺点：无。
  - B. 新 API 用 hplogc_（双前缀共存）——符号可区分新旧体系；缺点：同库
    两套前缀长期维护成本高。
  - C. 全库改名 hplogc——干净终态；缺点：全库破坏性迁移（宏、目录、配置
    词法、文档、存量用户代码全废）。
- **结论**：A。
- **理由**：改名收益不抵迁移成本；文档统一改写即可消除歧义。
- **影响**：全部新公共符号；rd_v0.6 文档措辞。

## D-S7 代码内配置的兼容策略（2026-09-28，Phase 5）

- **背景**：规范草稿「废除 output = 固定枚举 + 扁平结构体」，但
  `hpulogc_output_t` 是公共 API，存量代码与测试都在用。
- **选项**：
  - A. **垫片 + 通用声明**——扁平结构保留为 deprecated 垫片（内部映射
    console/rollingfile sink），同时向 `hpulogc_config_t` 追加通用 sink
    声明 `hpulogc_sink_decl_t{name, type, keys[], vals[], count}`，自定义
    sink 的代码内配置与 INI 完全对等。优点：存量零破坏 + 新体系全表达力
    + 后续新 sink 类型无需再扩扁平结构；缺点：两套声明并存一段时间。
  - B. 仅扁平结构垫片——自定义 sink 本期只能走 INI。优点：最简；缺点：
    代码内用户体验缺失。
  - C. 彻底废除不兼容——字面执行「已废除」。优点：无历史包袱；缺点：
    破坏源兼容，需主版本升级。
- **结论**：A（用户确认「为了后续支持更容易扩展实现更多的 sink 支持」，
  在可扩展性维度 A 最优）。
- **理由**：append-only ABI 规则允许追加字段；垫片使迁移期平滑；通用声明
  是「后续更容易扩展更多 sink」的承载点。
- **影响**：`hpulogc_config_t` 追加字段；conf 垫片映射；废弃标注。

## D-R1 热加载预存竞争（P-1/P-2）的修复策略（2026-09-28，独立缺陷工单）
- 背景：TSan 周任务自 Phase 4 起 `test_pipeline` 稳定报告两处数据竞争
  （state 普通读写、g_rt.conf 无锁读），main 的 schedule CI 因此持续
  标红；且排查确认 P-2 的并发对手是 shutdown 的 conf 释放（无锁），
  显式触发 reload 与 shutdown 并发时为 UAF 级风险，非仅 TSan 可见性。
- 选项：
  - A. TSan 抑制（注解/拦截器豁免）——改动最小 / 掩盖真实 UAF 风险、
    抑制规则随访问点漂移易失效、与仓库"先例仅用于良性重同步读"的
    豁免纪律相悖 / 代价：技术债持续存在
  - B. 仅加锁不改 shutdown——把 conf 读取入临界区即闭合 TSan 报告 /
    显式触发与 shutdown 并发时仍可能使用已释放快照（shutdown 不等
    reload） / 代价：修复不完整
  - C. state 原子化 + conf 指针临界区快照 + shutdown 等待在途 reload
    并持锁释放（选中）——语义显式化、竞争与 UAF 一并闭合、TSan 可
    建模 / 改动面稍大（全仓 ~40 处 state 访问点、机械替换）/ 代价：
    触碰核心生命周期路径，需全矩阵回归
- 结论：C。
- 理由：P-2 实为 UAF 而非纯可见性问题，抑制或半修都不诚实；A 违背
  仓库对 TSan 豁免的既有纪律（仅限良性重同步读，决策 4/9 先例）；
  B 留下真实的释放-使用窗口。C 的机械替换经 24 组合矩阵 + 双 TSan
  配置 + ASan/UBSan 全绿验证，风险可控。
- 影响：`core_internal.h`（state 字段与 load/store 辅助）、`core.c`
  （全访问点、stop 等待 busy、持锁释放 conf、清理误留 [DBG] 打印）、
  `hotreload.c`（入口快照 + 状态复核 + busy 访问器）；TSan nightly
  从此无预期内失败。同工单记录：顺手清理的 [DBG] 为 8b91072 误留。

## D-R2 macOS watcher 升级为 kqueue 后端的监视设计（2026-09-28，增强工单）
- 背景：implementation_notes 登记的遗留增强项——macOS 配置监视此前
  恒为 mtime/size 轮询，感知延迟受轮询间隔限制；spec 16.3 允许
  "kqueue 或轮询回退"。
- 选项：
  - A. 仅监视文件 vnode——实现最简 / 编辑器原子替换后旧 vnode 消亡，
    重新创建的文件无法感知（需靠 reload 失败轮询兜底）/ 代价：监视
    有空窗
  - B. 文件 vnode + 父目录 vnode 双监视（选中）——文件事件直接判定；
    kqueue 无法报告目录内变更条目名，目录事件用 stat 基线（mtime+size，
    复用 poll 后端的 hpu_poll_snapshot）确认相关性；DELETE/RENAME 后
    下一次等待自动重挂新 vnode，重建亦可感知 / 实现 比单监视略复杂 /
    代价：目录事件噪声需一次 stat 消解（可忽略）
  - C. 保持轮询不变——零风险 / 感知延迟无改善，与"可选增强"目标
    相悖 / 代价：无
- 结论：B；kqueue 不可用（fd 限制、沙箱）时透明回退轮询（spec 4.5）。
- 理由：B 与 Linux inotify 后端语义对齐（目录事件 + 名字过滤），
  感知延迟降到内核通知级，替换/重建全覆盖；回退路径保证可用性不
  劣于现状。
- 影响：`src/platform/darwin/darwin_watcher.c`（新）、
  `hpu_watcher.h`（append-only 追加 use_kqueue/vnode_dead 字段）、
  `posix_watcher_poll.{h,c}`（快照函数共享为 hpu_poll_snapshot）、
  CMakeLists darwin 源替换；macOS CI 全矩阵实测。

## D-R3 配置字段清单文件 conf_eg.ini 的存储位置（2026-09-29，文档工单）
- 背景：审查确认配置字段完整说明仅存在于 rd_v0.6 §10.3（嵌于 1300+ 行
  规格内），README 只覆盖常用子集，src/ 无面向用户说明；需要一份独立、
  可直接复制编辑的配置清单文件。
- 选项：
  - A. 仓库根目录 conf_eg.ini——发现性最强（zlog 惯例）/ 根目录混入
    非代码文件 / 代价低
  - B. examples/conf_eg.ini（选中）——与示例程序同目录，用户寻找示例
    的自然位置；根目录整洁 / 发现性略低于根目录 / 代价低
  - C. docs/conf_eg.ini——紧贴规范 / docs 为规格 markdown 目录，示例
    文件易淹没 / 代价低
  - D. 根目录 + docs 双份——两处可发现 / 双份内容必然漂移 / 代价高
- 结论：B，配套 README 文档索引与 rd_v0.6 §10.3 顶部指引（§10.3 为
  唯一规范源，副本随其同步）。
- 理由：B 在发现性与仓库整洁间平衡最佳；双份（D）漂移风险最高。
- 影响：`examples/conf_eg.ini`（新，全 9 节 60 键 + enabled 通用键
  示例，strict init + ASan/UBSan 实测可运行）；编制过程中实证修正
  rd_v0.6 §10.3 [formats] 模板缺陷（照抄内置五格式名会 duplicate
  启动失败）与 README 多 sink 示例的分号注释错误（`;` 非注释符）；
  登记 P-5（enabled=false 措辞 vs 实现语义）。

## D-R4 级别启用查询 API 的语义范围与未初始化返回值（2026-09-29，todo.md #3 工单）

- 背景：附录 A #3（level_enabled 检查 API）定案采纳，需确定两个语义自由度：
  ① 查询语义覆盖管线的哪些丢弃点（管线丢弃点依次为：状态检查 → 级别过滤
  §4.9② → 限流/采样③ → 溢出④ → 路由兜底⑥ → sink enabled 门禁⑧）；
  ② 未初始化/已 shutdown 时的返回值。约束：便捷宏用法为
  `if (HPULOGC_INFO_ENABLED(cat))`，非零即放行，负错误码会误放行。
- 选项（①语义范围）：
  - A. **仅级别过滤**（全局阈值 level_atomic + per-category override，
    与管线②精确一致）——优点：无假阴性（返回 0 则记录必被丢弃）、读路径
    零锁、实现最小（~40 行）；缺点：零输出误配置下返回 1 但实际丢弃
    （init 期已告警的边缘场景）/ 代价：无
  - B. 级别+路由（todo.md 字面建议）——优点：完整反映"是否会被输出" /
    缺点：每次门禁多一次 conf_lock 获取（与单条日志本身锁成本同量级，
    削弱门禁收益）；未注册 category 需决定注册副作用或直评规则；
    零输出仅误配置时出现，收益极小 / 代价：热路径锁竞争 + 代码增多
  - C. 含限流/采样预测——查询会消耗令牌/采样计数，查询改变后续行为，
    语义自我干扰，不可行（仅列具备完整性）
- 选项（②未初始化返回值）：
  - A. **恒返回 0（false）**——与"未初始化时日志必定不输出"语义一致，
    布尔门禁直接安全 / 代价：调用方无法区分"未初始化"与"被过滤"（非需求）
  - B. 返回 HPULOGC_ERR_STATE（-5）——与控制 API 惯例一致 / 缺点：负值
    在布尔门禁中误判为 enabled，宏内需层层 `== 1` 比较，易被第三方误用
  - C. 按默认级别 INFO 判断——未初始化时无配置可依据，语义捏造
- 结论：①A；②A。
- 理由：门禁宏的核心价值是"说 0 就一定被丢弃"的无假阴性 + 零锁热路径；
  "不预测路由"与 todo.md"不预测限流/采样"是同一逻辑延伸；未初始化恒 0
  与写 API 静默丢弃行为严格对齐。
- 影响：rd_v0.6 §4.1"级别启用查询"（v0.6.1）与 §7.3 条目；§17 P1 行；
  `hpulogc_level_enabled` 实现（registry find-only，无注册副作用）、
  `HPULOGC_TRACE_ENABLED`..`HPULOGC_FATAL_ENABLED` 宏族（受
  HPULOGC_COMPILE_TIME_LEVEL 裁剪，被裁级别恒 0）。

## D-R5 配置校验的入口形态（2026-09-29，todo.md #4 工单）

- 背景：附录 A #4（配置校验 CLI）定案采纳，入口形态决定 ABI 面与集成方
  体验。append-only ABI 规则允许追加公共 API；规范先行要求先在 rd_v0.6
  定义再实现。
- 选项：
  - A. 仅独立可执行 `hpulogc_chk_conf`（链接库内解析器）——优点：ABI 面
    零扩大、实现最快（main + load_file + finalize + 退出码映射）/ 缺点：
    集成方只能子进程调用（嵌入式/Windows 服务场景不友好）；错误文本仅
    stderr，无法程序化获取 / 代价：无
  - B. **新增公共 API `hpulogc_conf_validate(path, strict, err_buf, err_len)`
    + CLI 薄壳**（选中）——优点：集成方进程内预检（启动前校验是常见
    诉求）；CLI/CI/ctest 复用同一入口；后续 `hpulogc_strerror`（P2）可
    复用同一 diag 捕获机制 / 缺点：扩大 ABI 面（append-only 本为此设计）；
    需将解析诊断改造为可捕获（diag 回调，默认仍 stderr）；需给内部
    `hpu_conf_finalize` 加 dry-run 路径避免校验真实创建日志文件
- 结论：B。
- 理由：对集成方价值更高；diag 捕获是一次性投入且可复用；ABI 扩大在
  append-only 规则与规范先行流程内是常规操作。
- 影响：rd_v0.6 §7.3（conf_validate 条目）与 §11（CLI 小节，v0.6.1）；
  §17 P2 行；conf 模块 diag 回调改造 + finalize dry-run；tools/ 新 target
  （`HPULOGC_ENABLE_INI` 守卫，min 预设自动排除）；CI 校验
  examples/conf_eg.ini。CLI 细节（非决策项）：strict 默认跟随文件自身
  `strict init`（-1），`--strict/--lenient` 覆盖；退出码 0=合法/1=非法/
  2=用法或 IO 错误。

## D-R6 24h 压测的执行方式（2026-09-29，todo.md P1 发布前门禁）

- 背景：rd_v0.6 §13.3（注：todo.md 早期引用的"§13.5"实为自动化节，
  压测要求所在为 §13.3）要求"≥ 24h 高并发，验证无泄漏/无竞争/无死锁"，
  发布前门禁。GitHub-hosted runner 单 job 有 6h 时长上限。
- 选项：
  - A. GitHub Actions nightly 24h——环境干净、可归档 / 缺点：单 job 6h
    上限，需 4×6h 分片 + 跨 job 状态续传，复杂且状态不可靠 / 代价：高
  - B. **本地 WSL 后台 24h（先 1h 分片验证）**（选中）——优点：时长不受
    限；脚本 `--duration` 参数化，先 1h 验证脚本与判据采集正确再全量；
    nohup + 周期 checkpoint 支持断点续跑；会话内可跟踪 / 缺点：本机
    睡眠/WSL 重启风险（断点续跑缓解）；非生产环境（报告明示口径）
  - C. 缩短为 8–12h——不满足 §13.3 门禁时长，只能作为明示降级路径，
    不视为完成门禁
- 结论：B。
- 理由：A 受平台硬上限不可行；C 不满足规范门禁；B 配合断点续跑风险可控。
- 影响：`tools/soak/soak_driver.c`（EXCLUDE_FROM_ALL）+ `scripts/soak_24h.sh`；
  负载形态（非决策项，按 todo.md 定案）：MPMC 构建、consumer threads=4、
  4 生产者混合级别/category/结构化字段，sink 扇出 = rollingfile（size
  轮转）+ null + console 重定向文件，周期热加载（级别翻转）+ 周期 flush；
  通过判据（非决策项，按 §13.3 定案）：RSS 无增长趋势（无泄漏）、TSan
  零竞争、进度心跳无停滞（无死锁）、统计恒等式闭合；默认构建 + TSan
  构建并行。产出 `docs/perf_report_soak.md` + implementation_notes 登记。

## D-R7 hpulogc_strerror 缓冲区不足语义（2026-09-29，todo.md P2 工单）

- 背景：rd_v0.6 §7.3 新增错误描述 API（v0.6.3），缓冲区不足行为须在规范
  钉死。约束：库 API 面不依赖 errno（§7.4 规范性条款）、错误码集封闭
  （hpulogc_error_t 六值）、实现须线程安全无动态分配。
- 选项：
  - A. snprintf 语义——截断写入并返回完整描述所需字符数（不含 '\0'，
    ret >= len 即截断；buf=NULL 且 len=0 作长度查询）——优点：一次调用
    可探测所需长度；无 errno 依赖，与 §7.4 一致；两步法/一步法调用形态
    都自然 / 缺点：返回值是长度而非状态码，出错仅参数校验一种负值
    （buf=NULL && len!=0）；与 strerror_r 惯例不同 / 代价：无
  - B. ERANGE 语义——不足时返回负错误码（映射 ERANGE），截断串写入或
    未定义——优点：与 XSI strerror_r 惯例一致，返回值统一 0/负值 /
    缺点：须复用 HPULOGC_ERR_INVALID_ARG 兼表 ERANGE（语义混载）或新增
    错误码（破坏错误码集封闭性）；探测所需长度需迭代试探 / 代价：
    错误码语义污染或集合扩大
- 结论：A。
- 理由：hpulogc API 面明确不依赖 errno（§7.4），B 需破坏该原则或污染
  错误码集；snprintf 语义是 C23 标准化方向，长度查询形态调用便利。
- 影响：rd_v0.6 §7.3 条目（v0.6.3）；include/hpulogc.h 声明；
  src/core/api.c 实现；tests/unit/test_api.c 覆盖（全错误码/截断/
  长度查询/非法参数/未知码）；后续 hpulogc_chk_conf 诊断输出可复用。

### D-R6 注记（2026-09-29 追加：正式门禁执行环境变更，不覆盖原条目）

- 背景：原定案（选项 B）为本地 WSL 后台 24h。实际执行中发现宿主机存在
  中途关机风险，24h 门禁不可承受中断；用户决策改用云服务器。
- 变更：正式门禁迁移至云服务器 juzdata（Ubuntu 24.04，2核4G，内核 6.8，
  仓库 ~/hpulogc-soak，基线 4c687cd）；**WSL 双轨保留**作对照口径
  （原定案的负载形态、判据、产出物全部不变）。
- 影响：云上 TSan 变体须 `setarch $(uname -m) -R` 前缀（内核 6.8 高
  mmap_rnd_bits，环境不允许改 sysctl，同 soak 脚本 PR #18）；取数与
  三口径报告见 `docs/perf_report_soak.md`（待双轨完成后编制）。

## D-R8 v0.6.4 落地自由度定案：`%g` 缓存槽策略与修饰符支持范围（2026-09-29，工单「#6 `!LEVEL` 取反匹配 + #7 占位符扩展」）

- 背景：rd_v0.6 v0.6.4 规范先行完成后，任务书将两项实现层选择授权为自由度（"自定后登记"）：① `%g` 的时间缓存策略（独立槽或与 `%time` 共享槽位图）；② 占位符修饰符的支持范围钉死方式（规范要求"未列明的不支持"）。约束：稳态零 malloc（§9）不变、`%time` 与 `%g` 允许在同一模板并存、append-only ABI。
- 选项：
  - A. 缓存独立双槽（slot[0]=local、slot[1]=UTC，动作按 use_utc 选槽）——优点：改动最小，并存天然正确，缓存收益完整，稳态零 malloc 保持 / 缺点：每线程常驻内存约增 1KB / 代价：TLS 渲染存储的时间缓存部分翻倍
  - B. 缓存与 `%time` 共享单槽 + 位图区分——优点：内存不变 / 缺点：同格式并存时每条日志必有一个占位符退化为整 strftime 重渲染，按秒缓存收益减半；位图淘汰逻辑复杂 / 代价：热路径新增分支与比较，与 A.1 #1"每占位符独立缓存槽"的决策精神相悖
  - C. 修饰符范围 = 全部 10 个值占位符（level/time/g/pid/tid/file/line/func/msg/category），`%n`/`%%`/`%v` 不支持——优点：规则为一张封闭清单，可预测性最强 / 缺点：数值占位符对齐是低频场景 / 代价：无
  - D. 修饰符范围 = 仅字符串型占位符（level/time/g/file/func/msg/category）——优点：更贴近 printf 数值格式化惯例 / 缺点：需在规范中解释 pid/tid/line 为何被排除，清单出现人为例外 / 代价：可预测性下降、规范面变大
- 结论：缓存选 A；修饰符范围选 C。
- 理由：A 的常驻代价是每线程约 1KB 量级（相对 per-thread 渲染缓冲已存在的量级可忽略），换取缓存语义简单且与 A.1 #1 一致；B 无任何规范收益。C 与 D 的取舍在于规范可预测性——"封闭全清单 + 未列明不支持"是本文一贯基线（A.1 #7 同款），D 引入例外使规范面更大而收益存疑。
- 影响：src/format/format.h（`HPU_FMT_CACHE_SLOTS`、`hpu_fmt_action_t` 修饰符字段）、src/format/format.c（`render_time` 选槽、`parse_modifier`/`modifier_finish`）、src/core/pipeline.c（TLS 存储类型不变）、docs/rd_v0.6.md §10.3/§12（v0.6.4）、tests/unit/test_format.c、tests/unit/test_ini.c、tests/unit/test_conf_validate.c、tests/integration/test_pipeline.c；实现层明细见 implementation_notes「`!LEVEL` 取反匹配与占位符扩展（rd_v0.6 v0.6.4）实现自由度登记」（F-1..F-7）。
