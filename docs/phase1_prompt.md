# 任务：hpulogc Phase 1 —— Linux 全功能一次性实现与验证

## 1. 角色设定

你是一名拥有 15 年以上经验的资深 C 语言系统工程师，专长为无锁并发数据结构、跨平台抽象层设计与高性能日志/网络基础设施开发。你对以下方面负全责：

- **标准合规**：严格 C99/C11，无任何未定义行为，无 C++ 泄漏；
- **正确性**：线程安全、信号安全、内存安全（零泄漏、零竞争）；
- **性能**：量化指标达标，热路径零浪费；
- **工程完整性**：规范遵循、测试覆盖、文档与诚实汇报。

你的工作方式是**一次性自主交付**：从本提示词开始到任务结束，全程不向用户提问、不等待确认；所有规格未定义之处由你自行选择最合理实现并记录归档（见 §5）。任务只有在满足第 11 节验收清单后才允许结束。

## 2. 必读输入（写任何代码之前必须完整通读）

按顺序阅读，两者都是规范性输入：

1. `/home/ssy/proj/hpulogc/AGENTS.md` —— 项目编码规范（强制）：标识符 snake_case、注释仅英文、公共 API 全量 Doxygen（`@` 前缀）、源文件 UTF-8 无 BOM、`static` 限定可见性等。
2. `/home/ssy/proj/hpulogc/docs/rd_v0.2.md` —— 需求规格说明书 v0.2（规范性）。**这是实现的唯一依据**；实现与本档冲突时以文档为准。若你发现文档内部矛盾或未定义细节，自行选择与整体语义最一致的解法，并记录到 `docs/implementation_notes.md`（决策、理由、影响章节）。

本任务范围 = rd_v0.2.md **§16.1 Phase 1（Linux 全功能）**。当前工作区除文档外没有任何代码，一切从零创建。

工作环境：Linux x86_64（WSL2）。先检查可用工具链（`gcc`、`clang`、`cmake`、`valgrind`、`lcov`/`gcovr`、`doxygen` 等）；优先使用已有工具，缺失时可尝试包管理器安装，失败则采用降级方案并如实记录，不得因工具缺失阻塞或虚构结果。

## 3. 任务总览与交付物

| # | 交付物 | 依据 |
|---|--------|------|
| 1 | 完整源码树：`include/` + `src/`（按 §3.3 组织），实现 §4/§5/§7/§9/§10 全部功能 | §3.3、§4、§7 |
| 2 | 平台接口契约冻结：`src/platform/` 契约头；`src/platform/win32/`、`src/platform/darwin/` 仅含接口头与占位说明、不参与编译 | §3.3、§16.1 |
| 3 | CMake 构建体系：全部 `HPULOGC_*` 选项、`HPULOGC_C_STANDARD`、`HPULOGC_ATOMIC_BACKEND`、`HPULOGC_SANITIZER`、四版本预设、安装规则与 `hpulogcConfig.cmake` | §5、§11 |
| 4 | 测试体系：单元 + 集成 + fuzz + 性能基准，CTest 一键运行 | §13 |
| 5 | `examples/`：minimal、async、multi_category、custom_format 四个可运行示例 | §14 |
| 6 | `docs/perf_report.md`：性能测试报告（§8） | §8、§13.3 |
| 7 | `docs/code_structure.md`：目录结构说明（每个文件的作用） | 用户要求 |
| 8 | `docs/implementation_notes.md`：实现决策记录 | 本提示词 §5 |
| 9 | 最小可用 `README.md`（简介、快速开始、构建说明） | §14 |

## 4. 实现范围与模块顺序

严格按以下顺序实现（每完成一步立即编译 + 运行该模块相关测试，再进入下一步）：

1. **公共 API 头** `include/hpulogc.h`：与 §7.3/§7.6 逐字对齐——API 签名、`HPULOGC_LEVEL_*` 枚举、错误码、`hpulogc_config_t`/`hpulogc_output_t`/`hpulogc_rule_t`/`hpulogc_stats_t`/`hpulogc_build_info_t`、容量宏、版本宏、`HPULOGC_API` 导出宏、便捷宏与 `##__VA_ARGS__` 可移植方案（§7.3 说明）。公共 API 全量 Doxygen。
2. **平台契约头（Phase 1 冻结）**：`src/platform/` 下定义同步原语（mutex/cond 封装）、线程封装、高精度时钟与本地时间转换、文件打开/写/fsync/轮转（rename/unlink/目录扫描语义）、目录创建与权限、路径处理、watcher、线程 ID、导出属性等契约接口；`win32`/`darwin` 占位头仅含接口声明与占位注释。
3. **POSIX/Linux 平台层**：`src/platform/posix/`（与 macOS 共享）+ `src/platform/linux/`（inotify watcher；不可用时按 `hot reload interval` 轮询 mtime/大小回退）。
4. **原子层** `src/atomic/`：`stdatomic`、`gcc-atomic`、`gcc-sync` 三个后端 + 统一内联接口（§4.3）；同一测试套件以三种后端分别编译运行验证行为等价。
5. **环形缓冲** `src/ring/`：`ringbuf_locked.c` 与 `ringbuf_lockfree.c`（CMake 二选一编译）；支持 SPSC/MPSC；溢出策略 discard/overwrite/wait 及其构建约束（无锁 MPSC 下 overwrite/wait 配置必须启动失败，§4.3/§10.4）。
6. **核心管线**：§4.9 八步管线（编译期裁剪 → 级别过滤[覆盖语义] → 限流/采样 → 消息体渲染+入队 → 取出 → 路由 → 整行格式化 → 输出）；同步/异步两种模式；批量提交；category 登记表；stats 原子计数；shutdown 握手与超时（§7.5）。
7. **配置系统**：INI 解析器（§10.1 词法逐条实现：`#` 注释与行尾注释、引号感知、反斜杠续行、首个 `=` 分割、尺寸后缀 1k=1000/1kb=1024 族、物理行 ≤1024 字节、键名大小写敏感/值大小写不敏感）；节顺序校验与可选节（§10.2）；§10.4 行为表逐行实现（含 fail-fast 与 clamp+警告的区分）；热加载与 §10.5 行为明细（原子替换、失败整体回滚、未变更 file output 复用 fd、`[buffer]` 忽略）。
8. **格式化**：占位符预编译为动作序列（热路径禁止二次解析，§4.9）；5 个内置格式；`%f` 微秒/`%F3` 毫秒扩展；json 转义规则与 pid/tid 强制十进制（§12）；注入转义（§4.9⑦）；`max log length` 整行截断 + marker（§9）。
9. **输出**：console（stdout/stderr、颜色仅终端生效）、file（`file perms`/`dir perms`、轮转机制完整实现 §4.6：活动文件固定为 path、rename 到模板名、`{timestamp}`=`%Y%m%d_%H%M%S`、`{index}` 从 1 起不补零、时间桶边界 day/week[ISO 周一]/month、max files 清理、`.latest` symlink）；运行期写失败重开一次 + 限频 stderr 警告（§9）。
10. **throttle / signal-safe / fork 安全**：令牌桶（全局 + per-category 独立桶，先限流后采样）与确定性 1/N 采样（§10.3）；`hpulogc_log_signal_safe` 信号安全通道（§7.3）；`pthread_atfork` + fork behavior reinit/disable/inherit（惰性重建，§9）。
11. **CMake 体系**：`HPULOGC_ENABLE_*` 裁剪选项、`HPULOGC_LOCKFREE`、`HPULOGC_CONCURRENCY`、`HPULOGC_COMPILE_TIME_LEVEL`、`HPULOGC_C_STANDARD`、`HPULOGC_ATOMIC_BACKEND`、`HPULOGC_SANITIZER`、四预设（§5）、`-Wall -Wextra` 零警告、安装规则。
12. **示例** `examples/`：四个示例均可编译运行且输出格式正确。

## 5. 硬性约束

- 纯 C99/C11（由 `HPULOGC_C_STANDARD` 驱动）；**零第三方依赖**，仅 libc + OS 原生 API；禁止任何 C++ 特性或编译单元。
- AGENTS.md 全部规范强制执行；所有源文件 UTF-8 无 BOM；遵循根目录 `.editorconfig`（C 源码 4 空格缩进）。
- 平台差异只允许出现在 §3.2/§3.3 允许的位置（`src/platform/`、`src/atomic/` 后端头、ringbuf 二选一文件、`include/` 中的差异修补 `#ifdef`）。
- 性能红线：格式模板预编译、轮转检查 O(1) 不得逐条 `stat`、稳态热路径不调用 `malloc`/`free`（§4.9/§4.6/§9）。
- **禁止**修改 `docs/rd_v0.1.md`、`docs/rd_v0.2.md`、`AGENTS.md`；禁止任何 git 操作（init/add/commit/push 均不做）。
- **一次性自主完成**：不提问、不等待用户确认；规格未定义处自行决策并记录到 `docs/implementation_notes.md`。
- **诚实汇报**：测试失败、性能未达标、工具缺失必须如实写入报告与总结；禁止伪造数据、禁止为通过而放宽或删除测试断言。

## 6. 规范重点核查清单（实现完成后逐条自检）

以下条款在 v0.2 中有明确定义，是评审重点，逐条对照确认：

1. `%msg` 渲染（va_list 展开）必须发生在生产者线程，入队载荷 = 消息体 + 元数据（§4.9）。
2. per-category 级别为**覆盖语义**：已设置以 category 为准，未设置用全局（§4.1/§4.9②）。
3. 无锁 MPSC 构建下 `overflow policy = overwrite/wait` → 启动失败（§4.3/§10.4）。
4. `buffer size < 2 × max log length` → init 自动提升（下限 4KB）+ stderr 警告（§4.3/§9）。
5. 无命中规则 → 按 `default format` + `default outputs` 兜底；为空则丢弃（§4.9⑥/§10.3）。
6. json 格式仅做 JSON 转义（跳过注入转义），pid/tid 强制十进制（§12）。
7. 生产者渲染上限 = `max log length`（不加 marker）；整行截断 + marker 在格式化阶段（§4.9④/§9）。
8. `written` 按条计数（一日志多 output 仍计 1）；`throttled` 独立字段（§7.3）。
9. 轮转命名模板必须含 `{index}`/`{timestamp}` 之一，否则配置错误（§4.6/§10.4）。
10. 热加载原子替换 + 任一环节失败整体回滚；未变更 file output 复用 fd（§10.5）。
11. fsync 有效策略 = 全局 `crash_safety` 与 per-output `fsync` 取更严格者（none < shutdown < periodic < entry）（§9/§7.6）。
12. `signal_safe=false` 时 `hpulogc_log_signal_safe` 静默丢弃；函数始终 async-signal-safe 并保存/恢复 errno（§7.3/§9）。
13. 所有日志写入 API 保存/恢复调用线程 `errno`（§9）。
14. fork=reinit 惰性重建：atfork child handler 仅置原子脏标记，首次日志 API 调用时重建（§9）。
15. `HPULOGC_ENABLE_CATEGORY=OFF` 时规则按 `*` 匹配、`%category` 展开空串；`HPULOGC_ENABLE_SOURCE_LOC=OFF` 或运行时关闭时 `%file/%line/%func` 空串（§4.8/§12）。
16. 代码内配置必须经 `hpulogc_config_default()` 初始化；代码内配置非法值返回错误（不 clamp），配置文件非法值 clamp + 警告（§7.2/§10.4）。
17. throttle：先限流后采样；全局与 per-category 独立令牌桶；采样为全局共享确定性 1/N（§4.9③/§10.3）。
18. 所有跨线程计数走 §2.2 原子后端，禁止"事实原子性"（§9）。

## 7. 测试要求（§13 对齐，`tests/` 目录，全部接入 CTest）

**单元测试 `tests/unit/`**（覆盖率目标：模块行覆盖率 ≥90%，平台 glue 与 `#ifdef` 独占分支豁免并在报告中列出）：
- 环形缓冲：有锁/无锁两套实现 × SPSC/MPSC 全组合；功能正确性 + 并发压力（多线程生产者/消费者，断言无丢失、无重复、无乱序）；溢出策略行为与 `dropped`/`overwritten` 计数。
- 原子后端：同一测试套件分别以 `stdatomic`/`gcc-atomic`/`gcc-sync` 编译运行，断言行为等价。
- INI 解析器：§10.1 每条词法规则 + §10.4 行为表逐行断言（错误行号、节顺序、重复键、strict init 分支、尺寸后缀、clamp 与 fail-fast）。
- 格式化器：各占位符、时间格式扩展、json 转义、注入转义、截断与 marker、预编译后输出与模板一致。
- 轮转器：size/time/both 触发、命名模板与冲突递增、max files 清理、`.latest` 软链。
- stats 计数、生命周期语义（§7.5 表逐行）。

**集成测试 `tests/integration/`**：
- init 三种方式与错误路径、重复 init、shutdown 幂等（§7.2/§7.4/§7.5）。
- 端到端管线：规则路由命中（§10.3.1 示例表逐行验证）、兜底 default format/outputs。
- 热加载：合法变更、非法配置回滚、output 增删、fd 复用、`[buffer]` 忽略、SIGHUP 触发。
- fork：reinit（子进程可继续写日志）/disable/inherit。
- 磁盘异常：只读/满场景优先用注入 I/O 失败桩模拟，条件允许则用受限 tmpfs，方法记录到 notes。
- json 输出合法性（逐字段断言或解析校验）。

**模糊测试 `tests/fuzz/`**：INI 解析器 harness。有 AFL++/libFuzzer 则短时冒烟运行（1~2 分钟）；都没有则实现确定性随机变异 fuzz 程序跑固定轮数，入口保留 LLVMFuzzerTestOneInput 兼容形态；方式记录到 notes。

**性能基准 `tests/bench/`**：见第 8 节，基准程序纳入 CMake（独立构建目标，不进默认 ctest）。

## 8. 性能测试与报告（`docs/perf_report.md`）

基准程序必须逐项覆盖 §8 全部五个指标，**目标值以 rd_v0.2.md §8 为准**：

| 指标 | 场景 | 目标 |
|------|------|------|
| 单条延迟（无竞争） | SPSC + 无锁 + 64B 消息 | < 100 ns |
| P99 延迟 | MPSC + 无锁 + 8 生产者 | < 1 μs |
| P99 延迟 | MPSC + 有锁 + 8 生产者 | < 10 μs |
| 吞吐量峰值 | 64B/条、异步模式、批量同步写 | > 500,000 logs/sec |
| 基线内存 | min 版本（不含缓冲区） | < 32 KB |

方法学要求：
- `CLOCK_MONOTONIC` 计时；生产者-消费者场景用屏障同步起跑；预热（如 10k 条）后正式测量；每场景 ≥5 轮，报告取中位数/最优并注明；延迟给出 P50/P99/P999；报告**实测原始数据**，不得估算。
- min 基线内存：用 `/proc/self/statm` 差值或等价方法测量，方法写入报告。
- min 体积验证：min 预设 + `-Os` 构建，strip 后用 `size` 命令读取 `.text`，目标 ≤ 8KB（§5）。

报告结构：环境（CPU 型号/核数、内存、内核版本、WSL2 说明、编译器版本、CMake 选项与 flags）→ 方法学 → 结果表（指标 | 目标 | 实测 | 结论 pass/fail）→ 偏差分析（未达标项的原因分析与改进建议）→ 24h 压测标注为"发布前门禁，本次不执行"。

## 9. `docs/code_structure.md` 要求

- 完整目录树（代码块形式）+ **每个文件一行作用/职责说明**（覆盖 include、src 全部、tests 全部、examples、CMake 文件、scripts）。
- 平台契约冻结点清单：哪些头文件构成 Phase 2/3（Windows/macOS）的移植依据，各契约接口的职责。
- Phase 2/3 预留说明：`src/platform/win32/`、`src/platform/darwin/` 当前占位内容与后续填充计划。
- CMake 选项与四预设的简明使用说明。

## 10. 工作流程

1. 完整通读两份必读文档（第 2 节），不跳读。
2. 用 TodoWrite 建立任务清单（按第 4 节 12 步展开），随后逐项推进、实时更新状态。
3. 每个模块完成即编译（零警告）+ 运行相关测试；禁止把多个模块堆到最后一次性编译调试。
4. 全部实现后执行第 11 节验收清单，逐项确认。
5. 生成 `docs/perf_report.md`、`docs/code_structure.md`、`docs/implementation_notes.md`、`README.md`。
6. 输出最终总结（第 12 节）后结束任务，不留未完成承诺。

## 11. 最终验收清单（全部满足才可结束任务）

- [ ] 功能矩阵构建通过：{GCC, Clang} × {C99, C11} × {有锁, 无锁} × {SPSC, MPSC} 共 16 组合（提供 `scripts/run_matrix.sh` 并实际执行）；四版本预设（§5）各至少在一组编译器/标准下构建并 ctest 通过。
- [ ] 默认 full 构建 ctest 全部通过。
- [ ] GCC/Clang 下 `-Wall -Wextra` 零警告。
- [ ] `HPULOGC_SANITIZER` 开启 ASan+UBSan 后 ctest 零报告；TSan（工具可用时）核心并发测试零报告；Valgrind（可用时）零泄漏。工具不可用的情况如实记录。
- [ ] §8 五项性能指标逐项达标；任何未达标项在 perf_report.md 中有原因分析与改进建议，无隐瞒。
- [ ] min 预设 strip 后 `.text` ≤ 8KB。
- [ ] `docs/perf_report.md`、`docs/code_structure.md`、`docs/implementation_notes.md`、`README.md` 全部完成且内容与实际实现一致。
- [ ] 四个 examples 编译并运行成功，输出格式符合对应配置预期。
- [ ] 公共 API 全量 Doxygen 注释；doxygen 可用时零警告。
- [ ] 第 6 节 18 条核查清单逐条确认无违例。

## 12. 结束时的输出

任务结束前输出一份总结，包含：

1. 实现摘要（模块清单与关键设计决策，含 implementation_notes.md 中的决策索引）；
2. 测试统计（单测/集成数量、通过率、覆盖率数值、sanitizer/fuzz 结论）；
3. 性能报告要点（五项指标实测值 vs 目标、min 体积实测值）；
4. 交付文件清单（源码树概览 + 文档位置）；
5. 遗留事项与偏差（工具缺失降级、未执行项如 24h 压测、性能偏差及建议）。
