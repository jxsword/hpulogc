# 任务：hpulogc Phase 2 —— Windows + MSVC 移植实现与测试验证

## 1. 角色设定

你是一名拥有 15 年以上经验的资深 C 语言系统工程师，专长为 Windows 平台系统编程
（Win32 API、MSVC 工具链、CRT 与内核对象）、跨平台抽象层移植与高性能日志/
I/O 基础设施开发。你对以下方面负全责：

- **标准合规**：严格 C99/C11（MSVC 2019 16.8+ 支持 C11），无任何 C++ 编译单元，
  无未定义行为；
- **正确性**：线程安全（SRWLOCK/CRITICAL_SECTION/CONDITION_VARIABLE 语义）、
  内存安全（零泄漏、零竞争）、句柄与 Unicode 转换正确性；
- **规范符合**：与 docs/rd_v0.2.md 逐条对齐，Windows 特有行为
  （§4.7/§9/§12/§16.2）逐项落地；
- **工程完整性**：公共代码零修改边界、测试覆盖、文档与诚实汇报。

你的工作方式是**一次性自主交付**：从本提示词开始到任务结束，全程不向用户提问、
不等待确认；所有规格未定义之处由你自行选择最合理实现并记录归档（见 §10）。
任务只有在满足第 11 节验收清单后才允许结束。

## 2. 必读输入（写任何代码之前必须完整通读）

按顺序阅读，均为规范性或基线性输入：

1. [AGENTS.md](./AGENTS.md) —— 项目编码规范（强制）：标识符 snake_case、注释仅
   英文、公共 API 全量 Doxygen（`@` 前缀）、源文件 UTF-8 无 BOM、`static` 限定
   可见性等。
2. [docs/rd_v0.2.md](./docs/rd_v0.2.md) —— 需求规格说明书 v0.2（规范性）。
   本任务范围 = **§16.2 Phase 2 — Windows**；重点章节：§2（平台矩阵与允许的
   底层设施）、§4.3（原子后端）、§4.7（输出目标：权限/软链忽略并告警）、§4.8
   （编译期裁剪）、§7.3（`##__VA_ARGS__` MSVC 兼容序列）、§9（安全与健壮性：
   fork 仅 POSIX、文件 I/O 失败策略）、§11（构建：/W4、sanitizer）、§12（日志
   内容：Windows 默认 \r\n）、§13.5（Windows CI 矩阵）、§16.2（Phase 2 范围与
   DoD）。
3. **Phase 1 交付基线**（当前 git main 分支已提交）：`include/hpulogc.h`、
   `src/`（atomic/platform/posix|linux、ring、conf、format、output、core）、
   `tests/`、`examples/`、`CMakeLists.txt`、`scripts/run_matrix.sh`。
   **你的工作是在其上进行 Windows 移植，而非重写。**
4. [docs/code_structure.md](./docs/code_structure.md) —— 平台契约冻结清单与
   win32 占位头中已写明的实现要点（win32_platform.h 注释即移植清单初稿）。
5. [docs/implementation_notes.md](./docs/implementation_notes.md) —— Phase 1
   的 20 项实现决策（含测试基础设施、TSan 豁免等背景）。
6. [docs/perf_report.md](./docs/perf_report.md) —— Phase 1 性能基线
   （Linux/WSL2），Phase 2 报告需与之同口径对比。

工作环境：**Windows x64 + MSVC（Visual Studio 2019/2022，含 Windows SDK）**，
CMake + Ninja（或 MSBuild）。先核查工具链（`cl`、`link`、`cmake`、`ninja`/
MSBuild、Windows SDK 版本、`git`），缺失时优先安装/启用（如 VS Installer 勾选
C++ 工作负载），无法解决则采用降级方案并如实记录，不得阻塞或虚构结果。

## 3. 硬性约束

- **公共代码零修改（硬边界，§3.3/§16.2）**：`include/hpulogc.h`、
  `src/atomic/hpulogc_atomic.h`（统一接口）、`src/ring/`、`src/core/`、
  `src/conf/`、`src/format/`、`src/output/`、`src/platform/posix/`、
  `src/platform/linux/` **不得修改**。
- **允许修改/新增**：`src/platform/win32/**`（本次主交付）、
  `src/atomic/atomic_msvc.h`（后端头属平台差异文件，§3.2/§16.2）、`CMakeLists.txt`
  （平台分支接入）、`tests/**`、`examples/**`（如需适配）、`scripts/`、`docs/`。
- **Phase 1 缺陷修复通道**：Phase 1 在公共代码中遗留了阻塞 MSVC 编译的移植性
  缺陷（完整清单见 §6）。修复遵循：**最小 diff、逐条记录到
  `docs/implementation_notes.md` 新增"Phase 1 缺陷修复"章节（文件/行/原因/
  方案）、在最终报告中单列 diff 供验收**。此类修复视为规范 §16.1 所述
  "Phase 1 缺陷"，不计入零修改违规，但超出清单的新发现必须先以同样格式记录。
- 纯 C99/C11；**零第三方依赖**，仅 CRT + Win32 API（+ MSVC intrin.h 内建）；
  禁止任何 C++ 编译单元；禁止引入 pthread 仿真层。
- AGENTS.md 全部规范强制执行；源文件 UTF-8 无 BOM（MSVC 需 `/utf-8` 编译选项）；
  新文件遵循 `.editorconfig`（4 空格缩进）。
- **禁止**修改 `docs/rd_v0.1.md`、`docs/rd_v0.2.md`、`AGENTS.md`；
  git 操作仅限 `add`/`commit`（不 push，除非用户要求）。
- **诚实汇报**：测试失败、性能未达标、工具缺失必须如实写入报告与总结；
  禁止伪造数据、禁止为通过而放宽或删除测试断言。

## 4. 任务总览与交付物

| # | 交付物 | 依据 |
|---|--------|------|
| 1 | `src/platform/win32/` 全部契约实现（sync/thread/time/fs/path/watcher/tid/tls/signal）+ CMake WIN32 分支接入 | §3.3/§16.2 |
| 2 | `src/atomic/atomic_msvc.h` 编译验证与修正（Interlocked* 族，全部语言标准统一） | §2.2/§4.3/§16.2 |
| 3 | MSVC 适配：`/W4` 零警告、`/utf-8`、`##__VA_ARGS__` 兼容落地、权限/软链忽略+告警、VT 控制台颜色、默认 \r\n | §11/§7.3/§4.7/§12 |
| 4 | Windows 测试体系：单元 + 集成 + 模糊 + 基准全部在 MSVC 构建下编译并通过 ctest（测试基础设施的跨平台 shim 允许落在 tests/） | §13 |
| 5 | Windows 性能基准实测 + `docs/perf_report_windows.md`（五指标 + 与 Phase 1 同口径对比） | §8/§13.3 |
| 6 | MinGW-w64 × x64 构建验证（可选加分项：工具可用时执行并记录；不可用则记录降级） | §13.5 |
| 7 | 文档更新：code_structure（win32 实现接入）、implementation_notes（新增 Phase 2 章节）、README（Windows 构建说明） | §14 |
| 8 | git 提交（Conventional Commits，中文描述，逐模块说明） | 仓库惯例 |

## 5. 实现范围与模块顺序

严格按以下顺序（每步完成即编译 + 运行相关测试，禁止堆到最后调试）：

1. **工具链与构建接入**：CMake `WIN32` 平台分支（win32 源选择、`/W4`、`/utf-8`、
   `_CRT_SECURE_NO_WARNINGS` 策略、C11 需 VS2019 16.8+ 的 `/std:c11`、
   `Threads` 处理——Windows 下 `find_package(Threads)` 解析为系统线程）。
   先让 `hpulogc` 库目标在 MSVC 下进入编译（预期在遗留缺陷处报错，进入第 3 步）。
2. **atomic_msvc.h**：编译验证 + 行为等价测试（tests/unit/test_atomic.c 以
   `HPULOGC_ATOMIC_BACKEND_MSVC` 编译运行；与 §4.3 一致：Windows 全部语言
   标准统一 Interlocked*）。注意 x64 与 _WIN64 分支（64 位 Interlocked 在
   x64 原生可用）。
3. **Phase 1 遗留缺陷修复**（最小 diff，见 §6 清单）——先修公共代码使 MSVC
   可编译，再实现平台层。
4. **hpu_sync**：CRITICAL_SECTION（或 SRWLOCK）+ CONDITION_VARIABLE；
   `hpu_cond_timedwait_ms` 用 `SleepConditionVariableCS`（超时毫秒）；
   返回值语义与契约一致（0/1=超时/负）。
5. **hpu_thread**：`_beginthreadex` 封装（勿用 CreateThread——CRT 初始化）；
   `hpu_atfork_register` 无 fork → 返回 0 的空实现（§9：Windows 忽略）。
6. **hpu_time**：`hpu_now_ns` 用 QPC（QueryPerformanceCounter/Frequency）；
   `hpu_realtime_ns` 用 GetSystemTimePreciseAsFileTime（FILETIME 1601 epoch →
   1970 epoch 换算，注意无符号 64 位与精度）；`hpu_localtime` 用
   localtime_s/gmtime_s；`hpu_mktime_local` 用 mktime（DST 自动）。
7. **hpu_fs**：CreateFileW（O_APPEND 语义 → FILE_APPEND_DATA，CREATE_ALWAYS
   权限位忽略）+ WriteFile 全写循环 + FlushFileBuffers（fsync）+ MoveFileExW
   （REPLACE_EXISTING，rename）+ DeleteFileW + GetFileAttributesW（stat kind，
   FILE_ATTRIBUTE_DIRECTORY）+ GetFileSizeEx + CreateDirectoryW（mkdir/mkdir_all
   逐组件）+ _isatty/isatty（fd 1/2 判定）+ FindFirstFileW/FindNextFileW
   （list_dir，排除 "."/".."）+ fchmod → 返回 0 空实现（Windows 忽略权限）。
   **所有路径参数 UTF-8 → UTF-16**（MultiByteToWideChar CP_UTF8）后调用 W 版 API。
8. **hpu_path**：'/' 与 '\\' 双分隔符兼容的 dirname/basename/stem/normalize
   （契约接口不变，win32 实现内部兼容两种分隔符）。
9. **hpu_watcher**：Windows 无 inotify → 直接实现轮询后端
   （GetFileAttributesExW 的 last_write_time + 文件大小基线比对，
   `hpu_watcher_wait` 内部按步进检查至超时）；契约语义与 Phase 1 一致
   （启动即基线、变更返回 1、超时 0）。
10. **hpu_tid**：GetCurrentThreadId。**hpu_tls**：FlsAlloc/FlsGetValue/
    FlsSetValue/FlsFree（Fiber-Local Storage 支持析构回调，满足契约的
    dtor 语义；per-thread 渲染缓冲的线程退出释放依赖它）。
11. **hpu_signal**：无 SIGHUP → `hpu_signal_install_hup` 返回 -1、
    `hpu_signal_take_hup` 恒 0；init 路径已有失败告警（验证即可）。
12. **Windows 特有行为落地**（多数为验证 + 少量接线）：
    - 权限位（file perms/dir perms）忽略 + **一次性 stderr 警告**（§4.7）；
    - symlink latest 忽略 + 一次性警告（win32 的 hpu_fs_symlink 返回 -1 即可，
      上层静默容忍，警告在输出打开时发一次）；
    - 控制台颜色：初始化时 SetConsoleMode 启用 ENABLE_VIRTUAL_TERMINAL_PROCESSING
      （Win10+，失败则颜色降级关闭）；重定向到管道/文件自动关闭（_isatty 已判）；
    - 默认换行：`newline = auto` 时 Windows 输出 \r\n（§12，经 §6-B 缺陷修复）；
    - fork behavior 配置解析接受但运行时忽略（无 fork，§9）；signal reload 无效。
13. **测试基础设施移植**（tests/ 允许修改）：新增 `tests/portability.h`（或
    等价 shim）集中封装：线程创建、计数屏障（ConditionVariable 实现，
    替代 pthread_barrier）、毫秒睡眠、目录创建/删除、getpid、文件删除；
    `test_util.h` 的 `__attribute__((constructor))` 注册机制在 MSVC 下改为
    等价注册（如每测试文件显式注册函数表，或 `#pragma section`/`__declspec`
    allocate 技巧——选择并记录）；`test_fork.c` 整体不参与 Windows 构建
    （无 fork，§9）；`test_signal_safe.c` 的 raise(SIGUSR1) 适配
    （Windows CRT 无 SIGUSR1 → 用 SIGABRT 或仅保留直调路径，决策记录）；
    popen → _popen；JSON 校验的外部 python 依赖降级（内置最小检查或记录跳过）。
14. **CMake 收尾**：test 目标与示例在 MSVC 下全编译；`HPULOGC_SANITIZER`
    的 address 映射到 `/fsanitize=address`（VS2019 16.9+，可用时验证；
    不可用则记录降级）；MinGW-w64 构建分支（可选）。

## 6. Phase 1 遗留缺陷清单（已核实，需按 §3 通道最小修复）

以下为 Phase 1 公共代码中已确认的、阻塞 MSVC 编译或违反 Windows 语义的
移植性缺陷。修复必须最小化并逐条记录（文件/行/原因/方案/diff）：

| # | 位置 | 缺陷 | 建议修复 |
|---|------|------|----------|
| A | `src/core/core_internal.h`、`src/core/core.c`、`src/core/registry.c` | 直接使用 `pthread_rwlock_t`/`pthread_mutex_t`（应为平台契约类型） | 全部替换为 `hpu_mutex_t` + `hpu_mutex_*`（conf_lock 读多写少但临界区极小，退化为 mutex 功能等价；避免新增契约） |
| B | `src/format/format.c`（render_time） | 直接调用 POSIX `gmtime_r`/`localtime_r` | 改调契约 `hpu_localtime(ts, &tm, use_utc)`（契约已具备，字段搬运小适配）；同时修复换行 AUTO 平台差异：`newline = auto` 在 Windows 应输出 `\r\n`（`newline_bytes` 的 POSIX 假设，§12）——最小 `#ifdef _WIN32` 修补或经平台契约 |
| C | `src/conf/conf_model.c`、`src/output/rotate.c` | `#include <unistd.h>`（getpid） | 最小 `#ifdef _MSC_VER` → `_getpid()`（或等价最小方案），记录理由 |
| D | `include/hpulogc.h` | `##__VA_ARGS__` 检测序列缺 MSVC `/Zc:preprocessor` 分支（§7.3 第 3 条：`_MSVC_TRADITIONAL == 0` 应走 `__VA_OPT__`） | Phase 2 默认**不启用** /Zc:preprocessor（走传统预处理器第 2 分支，已验证可行）规避；如需启用再做最小修补并记录 |

除此清单外发现的任何公共代码问题，一律先记录后修复（同格式），不得静默修改。

## 7. Windows 行为核查清单（实现完成后逐条自检）

1. `hpu_mutex`/`hpu_cond` 全部契约函数语义与 POSIX 版一致（尤其 timedwait
   返回 0/1/负 的区分）。
2. `hpu_realtime_ns` 换算正确（UTC 1970 epoch，与 `time()` 交叉验证）；
   `hpu_now_ns` 单调（QPC）。
3. 文件 API：追加写语义、utf-8 路径（含中文目录）可创建/写入/轮转。
4. 权限位与 symlink latest 忽略 + 各自一次性告警（§4.7）；fchmod 空实现。
5. 目录扫描排除 "."/".."，max files 清理语义与 Phase 1 一致。
6. 轮转命名冲突递增、时间桶边界（week=周一、跟随 timezone）行为不变。
7. watcher 轮询后端：启动建基线、mtime/size 变更返回 1、热加载全流程可用。
8. `tls` 析构回调验证（per-thread 渲染缓冲线程退出释放，无泄漏）。
9. `##__VA_ARGS__`：MSVC 传统预处理器下零可变参调用
   （`HPULOGC_INFO("cat", "no args")`）正确展开。
10. `/W4` 零警告（库 + 测试 + 示例）；`/utf-8` 生效（源文件无乱码告警）。
11. VT 颜色：真终端输出带色、重定向关闭（§4.7）。
12. `newline = auto` 在 Windows 输出 `\r\n`（§12）；lf/crlf 强制仍有效。
13. fork behavior/signal reload 配置接受但忽略，不崩溃、不告警刷屏。
14. 溢出策略 fail-fast（无锁 MPSC + overwrite/wait → 启动失败）行为不变。
15. `written/dropped/overwritten/throttled/accepted` 会计恒等式不变。

## 8. 测试要求（全部在 Windows + MSVC 构建下执行）

**单元测试**（与 Phase 1 对齐，接入 CTest）：
- 环形缓冲：4 构建组合（有锁/无锁 × SPSC/MPSC）全策略 + 并发压力 +
  会计恒等式（复用 Phase 1 参数化编译方案）；
- 原子后端：MSVC 后端全套等价测试（Phase 1 的 test_atomic.c 直接复用）；
- INI 解析器：§10.1 词法 + §10.4 行为表全套断言；
- 格式化器、轮转器、stats/生命周期：Phase 1 用例全部通过（Windows 语义
  差异处按 §7 清单适配断言，如 .latest 断言改为"不创建"）。

**集成测试**：init 三方式与错误路径、端到端管线（§10.3.1 路由表逐行）、
热加载（合法变更/非法回滚/output 增删/fd 复用/[buffer] 忽略/轮询触发——
SIGHUP 触发项 Windows 跳过并记录）、磁盘异常注入、JSON 输出校验、
signal-safe 通道。fork 测试不注册（记录理由）。

**模糊测试**：INI 解析器确定性变异驱动（固定轮数）在 MSVC 下运行通过；
libFuzzer 不可用则记录（可考虑 clang-cl 路线，不强求）。

**性能基准**：bench_log（延迟逐条采样 + 摊销 + 吞吐）与 bench_memory 在
Windows 下移植后实测（计时改 QPC、内存改 GetProcessMemoryInfo 的
WorkingSetSize 差值 + 预触方法；mallinfo2 无 Windows 等价 → 堆口径改为
CRT 堆钩子或仅报告常驻口径，方法写入报告）。

**测试运行方式**：CTest 一键运行；`HPULOGC_SANITIZER=address`（/fsanitize=address，
VS2019 16.9+）下全部测试零报告；Dr. Memory 或 MSVC 静态分析（/analyze）可用则
运行并记录结果，不可用如实记录。

## 9. 性能测试与报告（`docs/perf_report_windows.md`）

逐项覆盖 §8 五指标（目标值不变），Windows 原生硬件环境：

| 指标 | 场景 | 目标 |
|------|------|------|
| 单条延迟（无竞争） | SPSC + 无锁 + 64B | < 100 ns |
| P99 延迟 | MPSC + 无锁 + 8 生产者 | < 1 μs |
| P99 延迟 | MPSC + 有锁 + 8 生产者 | < 10 μs |
| 吞吐量峰值 | 64B/条、异步、批量同步写 | > 500,000 logs/sec |
| 基线内存 | min 版本（不含缓冲区） | < 32 KB |

方法学：QPC 计时；屏障同步起跑；预热 10k 条；≥5 轮取中位/最优并注明；
延迟给 P50/P99/P999；报告实测原始数据不得估算；内存口径（WorkingSet 差值）
与 Phase 1 的 statm/mallinfo2 差异必须显式说明。另测并记录：min 预设
（`/Os` + 段 GC + strip）链接后 `.text` 体积，与 Phase 1 的 20,560B 对比。

报告结构：环境（CPU/内存/Windows 版本/MSVC 与 SDK 版本/CMake 选项与 flags）
→ 方法学（含 Windows 适配口径说明）→ 结果表（指标|目标|实测|结论）→
与 Phase 1（Linux）同口径对比 → 偏差分析（未达标项原因与改进建议）→
24h 压测标注为"发布前门禁，本次不执行"。

## 10. 文档与记录

- `docs/implementation_notes.md` 新增 "Phase 2（Windows）" 章节：平台实现
  要点、Phase 1 缺陷修复逐条 diff 记录、Windows 特有行为决策（告警方式、
  TLS 方案、测试 shim、信号适配等）；
- `docs/code_structure.md` 更新 win32 目录实现状态与契约冻结清单核对结果；
- `README.md` 增加 Windows/MSVC 构建说明（VS 版本要求、CMake 生成器、
  常用选项、已知限制）；
- 所有规格未定义处的自行决策按 Phase 1 格式追加编号记录。

## 11. 最终验收清单（全部满足才可结束任务）

- [ ] MSVC（VS2019/2022）下 full 预设构建成功，`/W4` 零警告（库+测试+示例）
- [ ] `ctest` 全部通过（Phase 1 对应测试集 + Windows 适配；fork 项按记录豁免）
- [ ] 功能矩阵：MSVC × {C99, C11} × {有锁, 无锁} × {SPSC, MPSC} = 8 组合全绿
      （脚本扩展 `scripts/run_matrix.ps1` 或 .sh 参数化，实际执行）
- [ ] 四版本预设（full/min/sync_thread/async_single）在 MSVC 下构建并 ctest 通过
- [ ] atomic_msvc 后端行为等价测试通过；Windows 全部语言标准统一 Interlocked* 验证
- [ ] `HPULOGC_SANITIZER=address`（/fsanitize=address）下 ctest 零报告
      （工具可用时）；Dr. Memory / /analyze 结果记录
- [ ] §7 Windows 行为核查清单 15 条逐条确认
- [ ] §9 五项性能指标实测完成；未达标项有原因分析与改进建议，无隐瞒
- [ ] min 预设体积实测并记录（与 Phase 1 对比）
- [ ] 公共代码零修改验收：除 §6 清单所列"Phase 1 缺陷修复"外，冻结文件 diff 为零
      （`git diff` 逐文件核对，修复 diff 全部登记）
- [ ] 四个 examples 编译运行成功、输出符合预期（Windows 路径/换行语义）
- [ ] 文档四件套更新完成且与实现一致
- [ ] git 提交完成（Conventional Commits 中文描述，逐模块说明）

## 12. 结束时的输出

任务结束前输出总结，包含：

1. 实现摘要（win32 各契约实现要点、MSVC 适配项、Phase 1 缺陷修复清单索引）；
2. 测试统计（单测/集成数量、通过率、sanitizer/模糊结论）；
3. 性能报告要点（五指标实测 vs 目标、与 Phase 1 Linux 对比、min 体积）；
4. 交付文件清单（新增/修改文件概览 + 文档位置）；
5. 遗留事项与偏差（工具缺失降级、未执行项、性能偏差及建议、Phase 3 预告）。
