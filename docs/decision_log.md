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

## D-R10 Windows watcher 通知化的契约扩展方式（2026-09-29，工单「Windows watcher 原生化 + MinGW CI + 分发基准 + 包管理 recipe」）

- 背景：`win32_watcher.c` 原为恒轮询（mtime/size + Sleep 分片），需以
  `ReadDirectoryChangesW` 对齐 Linux inotify / macOS kqueue 的通知语义
  （平台对称）。RDC 后端需要三样状态：父目录 HANDLE、OVERLAPPED 事件
  （含常驻通知缓冲，须跨 wait 调用存活）、后端标志；而
  `hpu_watcher_t`（src/platform/hpu_watcher.h）是三平台共享的冻结契约
  结构体（rd_v0.6 §16.1），现无字段可容纳。约束：wait 由 hotreload 以
  ≤200ms 步长驱动、join 后才 stop，无需停止事件；wait 1/0/-1 三态、
  基线语义、删除等效变化语义不得变。
- 选项：
  - A. 复用现有字段（fd 装 HANDLE、watch_fd 装 OVERLAPPED 事件、
    use_inotify 兼作 RDC 标志）——优点：契约头零改动、免规范修订 /
    缺点：x64 上 fd 是 int（4 字节）装 8 字节 HANDLE 必截断，技术上
    不可行；语义扭曲不可维护 / 代价：无
  - B. 新增平台字段（`void* dir_handle; void* rdc_io; int use_rdc;`，
    用 void* 保持头文件平台无关）+ 规范修订——优点：类型正确；与
    linux/darwin 的 use_inotify/use_kqueue 标志模式一致；结构体为内部
    布局（不安装、非公共 ABI），各平台只编译自己的 TU，无跨平台冲突 /
    缺点：触碰冻结契约，须走规范修订 / 代价：头 +8 行；hpu_watcher.h、
    win32_platform.h 注释、rd_v0.6 §4.5/§16.2 三处文档同步
  - C. 不通知化，仅缩短轮询间隔——优点：零契约风险 / 缺点：不解决
    平台对称目标，todo 工单无法关闭 / 代价：后续仍要做
- 结论：B。
- 理由：契约"冻结"是流程约定，`hpu_watcher_t` 实为内部实现细节
  （core_internal.h 持有、非安装头），扩展实际风险很低；任务书明确
  预期"契约需要小扩展，走规范修订"；A 有 x64 截断硬伤，C 不达目标。
- 影响：src/platform/hpu_watcher.h（字段扩展）、
  src/platform/win32/win32_watcher.c（RDC 后端 + 轮询回退 +
  运行时降级）、src/platform/win32/win32_platform.h 注释、
  docs/rd_v0.6.md §4.5/§16.2；热加载对外行为（§10.5）无变化。

## D-R9 包管理 recipe 方案：vcpkg 先行（2026-09-29，工单「Windows watcher 原生化 + MinGW CI + 分发基准 + 包管理 recipe」第 0 步定案）

- 背景：CMake `install(EXPORT)` + `hpulogcConfig.cmake.in` 分发骨架已
  具备，但库未被任何包管理器收录，外部消费者需手写 add_subdirectory
  或自造 port（todo P2"包管理 recipe"，rd_v0.6 §17/§16.3 预留）。目标：
  消费者可用 `find_package(hpulogc)`，CI 冒烟验证全链路，README 分发
  章节同步。
- 选项：
  - A. vcpkg——优点：与现有 CMake 安装零摩擦（直接消费 install 产物）；
    契合主受众（Windows/MSVC，本会话刚补齐 Windows 侧）；微软官方维护，
    manifest 模式改动量最小 / 缺点：消费面偏 Windows/CMake 用户；未进
    官方 registry 前外部用户需 overlay 或等 upstream port PR / 代价：
    1 个冒烟 CI 作业（约 +3–6 分钟）；portfile ~50 行；每个 release
    同步 port 版本
  - B. Conan——优点：跨平台消费面广（Linux/macOS/任意构建系统）；
    Conan 2 recipe 自包含、可纯本地验证 / 缺点：主受众是 CMake 用户，
    `conan install` 生成 toolchain 对纯 CMake 用户是额外概念；
    Conan 2 recipe API 有学习与升级维护成本；CI 需 pip 装 conan /
    代价：conanfile.py + test_package ~80–120 行；1 个冒烟作业；
    recipe API 演进风险
  - C. 两者都做——优点：覆盖全部消费人群；分发文档一次写全 / 缺点：
    双份 recipe 版本长期同步、CI 分钟数翻倍；外部需求尚无证据 /
    代价：2 个冒烟作业（+6–12 分钟/PR）
- 结论：A（vcpkg 先行）。Conan 挂账 todo，待真实需求出现再补。
- 理由：与库的 CMake-first 分发零摩擦、与 Windows 主受众契合、维护
  成本最低；B 的概念成本对主受众偏高；C 在无外部需求证据前维护面
  翻倍，属过度投入。
- 影响：新增 `ports/hpulogc/{vcpkg.json,portfile.cmake,usage}`（overlay
  port，REF/SHA512 随 release 同步 bump）、根 `vcpkg.json` +
  `vcpkg-configuration.json`（仓库内 manifest 冒烟路径）、
  `scripts/vcpkg_smoke/`（最小消费者工程）、
  `.github/workflows/vcpkg_smoke.yml`（windows-2025 冒烟作业）、
  README"安装与集成"章节 vcpkg 小节。

## D-R11 外部轮转检测的节流参数 N（2026-09-30，工单「#5 外部轮转检测 + bench_sink 常态化性能回归」）

- 背景：附录 A.1 #5 已决策"节流方案（每 N 条 + 写失败时比对 inode）"，
  但 N 的取值与可配置性未定。N 越小检测窗口越短、fstat 开销占比越高；
  测试需跨过 N 才能覆盖检测路径。
- 选项：
  - A. 固定常量 1024（output_file.c 内 static const，不可配置）——优点：
    零配置面、规范条款最简（"固定 1024"一句话）、实现最少；测试写
    ~2100 行即可覆盖，成本可忽略 / 缺点：极端场景（低频日志 + 频繁
    logrotate）检测延迟最多 1024 条 / 代价：无
  - B. 可配置（新增输出配置键，如 `rotate check every`）——优点：用户可
    按场景调小/调大 / 缺点：配置面扩大（键名、校验、文档、output.c 映射
    链三处改动），绝大多数用户不会触碰 / 代价：规范 + 配置模型 + 映射链
    维护成本
  - C. 固定但暴露编译期宏——优点：编译期可调 / 缺点：无运行期价值，
    规范仍需解释宏语义，半吊子方案 / 代价：宏语义登记 + 文档解释
- 结论：A（固定 1024）。
- 理由：logrotate 典型轮转周期远大于 1024 条的写入窗口，检测延迟可接受；
  任务书授权实现自由度（"建议每写 1024 条或可配置"），A 的收益/成本比
  最高；B 的配置面扩散无对应真实需求；C 兼具 A 的不灵活与 B 的文档成本。
- 影响：rd_v0.6 §4.6（v0.6.5）、src/output/output_file.c 常量、
  docs/implementation_notes.md 自由度登记。

## D-R12 Win32 打开文件共享模式扩展（2026-09-30，工单「#5 外部轮转检测 + bench_sink 常态化性能回归」）

- 背景：外部轮转检测的测试需模拟 logrotate（mv 已打开文件 + 新建同名）。
  win32_fs.c 的 `hpu_fs_open_append` 以 `FILE_SHARE_READ` 打开，外部进程
  重命名/删除会共享冲突失败——不扩展共享模式，外部轮转在 Windows 上无法
  被任何外部工具触发，检测在该平台形同虚设。
- 选项：
  - A. 共享模式扩为 `FILE_SHARE_READ | FILE_SHARE_WRITE |
    FILE_SHARE_DELETE`——优点：特性在 Windows 真实可用；测试双平台跑齐；
    与 POSIX 打开语义（外部进程可自由 rename/unlink）对齐；库本就不依赖
    独占访问 / 缺点：改变现有平台行为（外部进程可在句柄持有期间重命名/
    删除文件——但这正是本特性要检测的场景） / 代价：win32_fs.c 一处改动
    + 行为注记登记；需验证 FlushFileBuffers（fsync）不受影响
  - B. 保持现状，测试 POSIX-only——优点：零行为变化 / 缺点：Windows 上
    检测无法被外部触发；测试双平台不对齐；规范需写平台差异条款 / 代价：
    规范复杂化、平台上限
  - C. 仅测试内绕过（测试用私有 API 打开）——优点：不动库代码 / 缺点：
    测试模拟失真（真实 logrotate 场景在 Windows 仍不可用），私有 API
    泄漏进测试 / 代价：不诚实的覆盖
- 结论：A。
- 理由："logrotate 互操作"要在 Windows 完整落地就必须允许外部替换已打开
  文件；POSIX 语义本就如此，A 是对齐而非放宽；fsync 经 FlushFileBuffers
  不依赖共享模式。B 让特性在 1/3 目标平台失效，C 是假覆盖。
- 影响：src/platform/win32/win32_fs.c（共享标志 + 头注释）、
  docs/rd_v0.6.md §4.6（v0.6.5 平台注记）、tests/unit/test_ext_rotate.c
  双平台运行。

## D-R13 bench_sink/bench_log 夜间回归作业的频率与时刻（2026-09-30，工单「#5 外部轮转检测 + bench_sink 常态化性能回归」）

- 背景：CI 现有 schedule 仅有每周一 21:00 UTC 的 TSan 夜间作业
  （ci.yml），bench 目前仅手动 bench_dispatch（windows/macos，无阈值
  判断）。任务书要求"夜间常态化回归、与 TSan 错峰、>25% 相对阈值"。
- 选项：
  - A. 每日 02:00 UTC——优点：回归发现最多滞后一天；与周一 21:00 UTC
    天然错峰；ubuntu-latest 公共 runner 成本低，25% 相对阈值吸收
    runner 噪声 / 缺点：runner 用量最大（7×/周） / 代价：每日一次
    bench 作业（分钟级）
  - B. 每周二 02:00 UTC（每周）——优点：省额度；紧随 TSan 之后一天 /
    缺点：回归发现滞后最多一周，"夜间常态化"名不副实 / 代价：覆盖弱
- 结论：A（每日 02:00 UTC）。
- 理由：性能回归的价值在"及时发现"，每日调度与任务书"常态化"表述一致；
  错峰天然满足（时刻不相交）；runner 成本为分钟级 ubuntu 作业，可忽略。
  基线建立采用 bootstrap：PR 合并后 workflow_dispatch 首跑 → 数值转录
  进仓库内 tests/bench/baseline_linux.json → 此后每晚自动比较，基线缺失
  时脚本告警放行（不阻塞 CI）。
- 影响：.github/workflows/ci.yml（schedule + bench-nightly job）、
  scripts/bench_gate.py、tests/bench/baseline_linux.json、
  docs/perf_report_sinks.md（基线建立流程）。

## D-R14 per-sink 字段过滤的取舍与键语义（2026-09-30，会话 5「远期项取舍 + 选做实现」）

- 背景：§4.10.3 明文预留 per-sink `fields_dropped` 恒 0（"为未来
  per-sink 字段过滤预留"），是 todo.md 远期项中唯一有规范义务的项。
  v0.6.2 网络型 sink（tcp/udp，PR #19）落地后，向低带宽/按量计费链路
  按 sink 裁剪字段的价值变得真实；实现地基已具备（sink 经 kv 对接收
  配置、字段数组已随事件到达 emit 路径、槽位已预留）。
- 选项：
  - A. 做并本会话实施——优点：兑现唯一规范义务，网络 sink 场景价值
    真实 / 缺点：六项中工作量最大（规范增补 + output 分发路径 + 记账 +
    测试）/ 代价：一个完整工单容量，不足部分顺延；风险：低-中（ABI
    无破坏，字段已预留）
  - B. 做但降级（仅规范增补）——优点：稳妥 / 缺点：本会话无代码交付 /
    代价：拖长兑现周期
  - C. 不做——优点：无 / 缺点：需反向修改 §4.10.3 删除"预留"措辞，
    反而新增规范工作 / 代价：为放弃而返工，不成立
- 结论：A。
- 理由：唯一规范义务项 + 网络 sink 落地后的真实边际价值，B 与 C 均
  无对应收益面。
- 设计自由度（随条目登记，D-R14 附带结论）：
  - 键形态：白名单通用键 `filter keys`，值为逗号分隔字段名列表
    （inline 场景按 §10.1 引号包裹，如 `filter keys="a,b,c"`）；
  - 归属层：放 §4.7.3 通用键（核心消费、挂在 output 实例上），所有
    内置 sink 统一生效，避免五个 sink 各自重复解析；
  - 过滤位置：sink 分发路径——同步 sink 在 emit 前构造过滤视图（不改
    写共享 staging），异步 sink 在 per-sink queue worker 解码后、
    emit_batch 前同样处理；无过滤配置时走零开销快路径（指针判空直通）；
  - 记账：per-sink `fields_dropped` 只记因过滤丢弃的字段数；生产端
    §4.11.2 三成因（预算裁剪）仍只进全局统计，二者正交、不重复计数。
- 影响：docs/rd_v0.6.md §4.7.3/§4.10.3、docs/mul_sink.md、
  src/conf/conf_model.c、src/output/（分发与统计）、tests。

## D-R15 附录 A.1 #13 shutdown/热加载线程协同强化：降级（2026-09-30，会话 5）

- 背景：现机制（`closed` 原子标志 + condvar 有界等待 + watcher join/
  6s busy 槽）已满足 §7.5/§10.5 行为定义，附录 A.1 定性为"健壮性
  强化"（队列内命令记录 + 条件变量握手 + 生产者引用计数延迟释放）。
  tcp/udp 重连线程加入后协同复杂度上升，但无行为契约缺口。
- 选项：
  - A. 做并本会话实施——优点：一次到位 / 缺点：与 24h 压测收尾直接
    冲突——shutdown 是最敏感并发路径，改动即令 24h 证据链作废；风险
    与测试成本六项最高 / 代价：压测整链重跑
  - B. 做但降级（保留 todo 远期）——优点：不与压测收尾冲突，保留已
    采纳决策 / 缺点：强化延后 / 代价：无即时
  - C. 不做并移出 todo——优点：无 / 缺点：与附录 A.1 已有"采纳"决策
    矛盾 / 代价：决策记录返工
- 结论：B（降级）。
- 理由：时点不合适（压测今日收尾）且无契约缺口，但既定采纳决策不应
  撤销；压测归档（perf_report_soak.md 落档、P-6 登记）后由后续会话
  排期。
- 影响：todo.md 远期区措辞更新（标注压测归档后排期）。

## D-R16 附录 A.1 #12 双映射环形缓冲：不做（2026-09-30，会话 5）

- 背景：原评估假设现环存在跨边界记录的双拷贝成本。会话 5 探查证实：
  lock-free 环已用 PAD 记录方案保证记录连续——生产者在跨界时发布
  `HPU_REC_FLAG_PAD` 记录并回到头部写入（ringbuf_lockfree.c
  `hpu_ring_put`），单次 memcpy；消费者单次 memcpy + seqlock 复验。
  双映射环相对现实现的唯一增益是省掉 PAD 记录的微量空间与一条 header。
- 选项：
  - A. 做并本会话实施——优点：无实质 / 缺点：收益≈0，引入
    memfd_create/MAP_FIXED 平台专属分支，破坏"平台抽象集中于
    atomic/platform"的现结构 / 代价：平台复杂度永久增加
  - B. 做但降级——优点：无 / 缺点：同 A 而更拖延 / 代价：同 A
  - C. 不做（移出 todo，结论留档）——优点：消除无收益的平台分支 /
    缺点：附录 A.1"采纳（可选优化）"决策被推翻 / 代价：本条记录
- 结论：C（不做）。
- 理由：其优化对象（跨边界双拷贝）在现实现中不存在；附录 A.1 定性
  为"可选优化"，优化对象消失则采纳前提消失，推翻不构成违约；规范
  正文仅要求跨平台回退语义不受双映射影响，无需改动。
- 影响：todo.md 移除该项（本条记录保留探查结论供回溯）。

## D-R17 JSON/YAML 配置格式：不做（2026-09-30，会话 5）

- 背景：§4.5 配置系统表格中为"预留接口，后续支持 JSON/YAML"——措辞
  是预留而非承诺。与零依赖定位张力最大：YAML 全集自研解析成本极高，
  JSON 单做也需约 600+ 行解析器与完整测试；无任何需求信号；现有
  zlog 兼容 INI 词法是生态兼容资产。
- 选项：
  - A. 做（JSON-only）——优点：兑现预留字面 / 缺点：纯增负，边际
    价值低 / 代价：解析器 + 测试 + 规范 + 三格式互斥语义
  - B. 做但降级——优点：无 / 缺点：持续挂账 / 代价：同 A
  - C. 不做——优点：守住零依赖定位 / 缺点："预留"措辞悬空 / 代价：
    需将措辞降级（随本会话 docs PR 搭车完成）
- 结论：C（不做），并将 §4.5 措辞由"预留接口，后续支持 JSON/YAML"
  降为"可选扩展方向（无排期）"，消除悬空承诺。
- 理由：零依赖是库的定位资产；无需求信号时自研解析器是纯负债；
  降级措辞后"不做"与规范一致。
- 影响：todo.md 移除该项；docs/rd_v0.6.md §4.5 措辞修订（随 D-R14
  的 docs PR）。

## D-R18 macOS universal binary：做（2026-09-30，会话 5）

- 背景：CI 已覆盖双架构（arm64 原生多作业 + x86_64 经 Rosetta 的
  macos-arch 作业），但无单文件 universal 工件。规范明确"可选、非
  §16.3 DoD 要求"；本项目以源码分发 + vcpkg 交付，二进制工件需求弱
  但偶有真实需求（直接取用产物者）。
- 选项：
  - A. 做并本会话实施——优点：成本极低（一个 lipo 作业），补齐唯一
    登记的遗留增强 / 缺点：无 DoD 义务 / 代价：CI 一个 job
  - B. 做但降级——优点：容量让给 D-R14 / 缺点：近零成本项继续挂账
  - C. 不做——优点：无 / 缺点：成本近零而放弃略显浪费 / 代价：
    遗留项悬挂
- 结论：A（用户定案升级为当场实施）。
- 理由：成本近零，与 D-R14 实现无耦合，可并行完成。
- 影响：.github/workflows/ci.yml（lipo 合并 + `lipo -info` 与冒烟
  验证 job）。

## D-R19 Linux arm64 原生 CI runner：做（2026-09-30，会话 5）

- 背景：Linux CI 仅 x86_64（ubuntu-24.04），无 arm64 原生覆盖。
  原任务书假设的"runner 资源可得性"障碍经查已消除：仓库为 public，
  `ubuntu-24.04-arm` 对 public 仓库免费。代码用标准原子后端，原生
  风险低（对齐、原子序行为）但非零。
- 选项：
  - A. 做（进门禁）——优点：零费用补齐覆盖，作为门禁有约束力 /
    缺点：runner 偶发排队慢 / 代价：ci.yml 一个 leg + timeout 设置
  - B. 做但降级（continue-on-error）——优点：不阻塞 / 缺点：可选
    job 无门禁价值，"要么进门禁要么不做" / 代价：假覆盖
  - C. 不做——优点：无 / 缺点：免费且低成本，放弃理由不足
- 结论：A。
- 理由：免费、低风险、补齐平台矩阵的最后一格；B 是假覆盖。
- 影响：.github/workflows/ci.yml（linux-arm64-smoke leg）。

## D-R20 FIFO 打开语义：ENXIO（无读取者）延迟退避重试，不 fail-fast（2026-09-30，网络 sink 第二期「UDS/FIFO」任务书 §三.B）

- 背景：`fifo` sink（rd_v0.6 §4.10.9，v0.6.7）只开已存在 FIFO 的写端。
  POSIX 语义下，`open(FIFO, O_WRONLY | O_NONBLOCK)` 在对端读取进程尚未
  启动时报 `ENXIO`；而读取进程晚于（或独立于）日志使用方启动/重启是
  rotatelogs/multilog 类场景的常态。TCP/UDS 的 `start` 均为 fail-fast
  （首连失败 = `HPULOGC_ERR_IO`），FIFO 需要单独定夺打开语义。
- 选项：
  - A. ENXIO 延迟退避重试（不 fail-fast）——优点：读取进程晚启动是
    正常时序而非配置错误，实例照常建立、emit 路径按退避周期重试打开，
    与外部进程的生命周期解耦；与 EPIPE 重开共用同一退避机件，实现单一
    路径 / 缺点：纯配置错误（路径指向无人会读的 FIFO）不再于 start 时
    暴露，只能靠 failed 计数观察 / 代价：start 语义与 TCP/UDS 不一致，
    需规范明示差异与理由
  - B. start fail-fast（同 TCP：ENXIO 即 init 失败）——优点：全家族
    start 语义一致，配置错误早暴露 / 缺点：把"读取进程还没起"这一时序
    正常场景变成启动失败，rotatelogs 类场景要求严格的进程启动顺序
    （先读后写），部署脆弱 / 代价：使用者需自行编排启动顺序或重试逻辑
  - C. 懒连接（start 不打开文件，emit 时才试）——优点：start 永不因
    ENXIO 失败，也无 B 的顺序耦合 / 缺点：丢失"路径存在性/是否 FIFO"
    的启动期校验（ENOENT/非 FIFO 也推迟到运行期），`ENXIO` 与 `ENOENT`
    两类错误在记账上不可区分，可观测性最差；且与 rollingfile/TCP 的
    "init 即打开"基线偏离最大 / 代价：调试配置错误从启动期推迟到运行期
- 结论：A（任务书 §三.B 定案）。
- 理由：FIFO 的读取方是外部进程，晚启动属正常时序，不应 fail-fast；
  但路径校验（存在且为 FIFO）保留在 start（ENOENT/非 FIFO 仍
  fail-fast），折中 B 的"配置错误早暴露"与 A 的时序解耦——错误按
  类型分流而非一刀切。C 因可观测性最差被放弃。
- 影响：rd_v0.6 §4.10.9 fifo 语义条款（fail-fast 与延迟重试的分界）、
  `src/output/sink_fifo.c` start/emit 实现、§10.3 配置模板注释；
  实现层自由度（SIGPIPE 抑制、ENXIO/EPIPE 共用退避机件等）另见
  implementation_notes「IPC 型 sink」节。
