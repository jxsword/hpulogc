# hpulogc 实现决策记录（implementation notes）

本文记录 rd_v0.2.md 未明确定义处或实现层自由度上的选择、理由与影响章节。
所有决策均为 Phase 1（Linux）落地版本；Phase 2/3 移植时除特别注明外不得回退。

## 决策索引

| # | 决策 | 影响章节 |
|---|------|----------|
| 1 | 代码内配置输出的隐式命名 | §7.6 |
| 2 | fork 子进程内重初始化锁 | §9 |
| 3 | `hpu_ring_discard()`（fork 子进程专用） | §3.3/§9 |
| 4 | 无锁 SPSC overwrite 的 seqlock 协议与 TSan 豁免 | §4.3 |
| 5 | 注入转义按规范字面实现（\n、\r → 文本 `\n`） | §4.9⑦ |
| 6 | strict init 两遍解析 | §10.4/§10.5 |
| 7 | 缓冲区自动提升公式含元数据余量 | §4.3/§10.4 |
| 8 | 时间戳捕获策略（REALTIME 恒捕获） | §4.9/§12 |
| 9 | TSan 构建下的测试豁免 | §13 |
| 10 | `json` 名字承载 JSON 语义；`%F<n>` 扩展 | §12 |
| 11 | INI=OFF 的裁剪面 | §4.8/§5 |
| 12 | min 预设体积优化开关 | §5 |
| 13 | 覆盖率豁免清单 | §13.1 |
| 14 | 注册表 count 发布协议 | §4.9 |
| 15 | I/O 失败注入钩子（仅测试） | §9/§13.2 |
| 16 | 写失败后 written→dropped 的统计迁移 | §7.3/§9 |
| 17 | 热加载三阶段（读快照→写锁搬迁→换后释放） | §10.5 |
| 18 | `stats interval` 在同步构建下忽略并告警 | §10.3 |
| 19 | `set_level_for_category` 清除覆盖的哨兵值 | §7.3 |
| 20 | init 解析的 strict 语义 | §10.4 |
| 21 | 零输出路由的丢弃计入 retired-lost（Phase 2 重建 core 时确立） | §7.3 |
| 22 | 同步构建 flush 语义为 best-effort（Phase 2） | §7.3 |
| 23 | Windows 二进制模式无条件切换（Phase 2） | §12 |
| 24 | Windows JSON 校验降级为内置检查（Phase 2） | §13.2 |
| 25 | MPMC 单共享环 + N 对称消费者，记录恰好一次投递（Phase 4） | rd_v0.3 §1.1 |
| 26 | `[async] consumer threads` 配置键与构建门禁（Phase 4） | rd_v0.3 §1.3 |
| 27 | 无锁 MPMC commit 字租约认领协议，discard 路径 TSan 干净（Phase 4） | rd_v0.3 §1.2/§5 |

## 1. 代码内配置输出的隐式命名（影响 §7.6）

`hpulogc_output_t` 没有 name 字段，而 `hpulogc_rule_t::outputs` 按
"output 名称数组" 引用（§7.6 原文存在此不一致）。**决策**：代码内配置的
输出按数组位次获得隐式名 `out0`..`outN-1`，规则与 `default_outputs` 以
这些名字引用。配置文件路径不受影响（[outputs] 的键即名字）。

## 2. fork 子进程内重初始化锁（影响 §9）

POSIX 只保证 fork 后调用线程存活；父进程其他线程若在 fork 瞬间持有
互斥量/读写锁，子进程内将永远锁死。atfork child handler（async-signal-
safe 约束下仅置脏标记是不够的）因此**同时重初始化**运行时的互斥量与
conf_lock。对已锁对象调用 destroy/init 严格说未定义，但这是 glibc 上
日志库处理 fork 的通行实践（zlog 同类）。文档化为已知权衡。

## 3. `hpu_ring_discard()`（影响 §3.3/§9）

同类问题作用于环形缓冲自身的条件变量：父进程消费者线程可能在 kick
cond 上等待。子进程重建时调用 `hpu_ring_discard()` 释放内存但**跳过**
原语销毁（销毁有等待者的 condvar 会阻塞）。仅 fork 子进程路径使用。

## 4. 无锁 SPSC overwrite 的 seqlock 协议（影响 §4.3）

无锁环的 overwrite 需要"覆盖最旧未消费记录"，物理上必然与慢消费者的
在途读取竞争。协议：生产者覆盖前先作废 commit 标记（store 0）再 CAS
推进消费游标；消费者拷贝前后双重校验标记（seqlock 读），失配则丢弃本
次拷贝重试。**该协议在逻辑上无撕裂输出，但物理数据竞争是设计固有**，
ThreadSanitizer 无法建模——TSan 构建下相关测试用例编译期跳过（见决策
9）。无锁 discard 路径完全 TSan-clean。会计：overwritten 只由生产者
CAS 成功计数，消费者 CAS 失败即放弃拷贝，杜绝重复计数。

## 5. 注入转义按规范字面实现（影响 §4.9⑦）

规范 §4.9⑦ 原文："换行符（\n/\r → \n 字面转义文本）"。按字面实现：
消息体中 `\n` 与 `\r` 均替换为两字符文本 `\n`（`\r` 信息丢失）。若
后续修订希望保留 `\r` 区分，改动点唯一：`format.c` 的
`rb_append_escaped_injection()`。

## 6. strict init 两遍解析（影响 §10.4/§10.5）

规范要求未知键的处理跟随 `strict init`，而该键位于文件 [global] 节内，
可能晚于其他节出现。实现为**两遍**：预扫描只提取 `strict init` 值，
正式解析以该值执行（`hpu_conf_load_file(c, path, -1)` 时）。显式传入
strict 的调用（热加载用当前生效值，§10.5）跳过预扫描。init 公共路径
传 -1，即 init 时文件自身的 strict 值治理自身解析。

## 7. 缓冲区自动提升公式（影响 §4.3/§10.4）

规范公式 `max(4KB, 2 × max log length)` 未含元数据余量。实现按
`max(4KB, 2 × max_log_length, 2 × max_record_size)` 提升，
`max_record_size = 48B 头 + max_log_length + 1KB 辅助串（category
256 + file 512 + func 256）`，保证任何单条日志（含路径/函数名）必可
入队——与规范"消除 wait 策略死锁边界"的意图一致。默认配置（4KB
消息）下提升阈值 10,368 字节并输出一次 stderr 警告。

## 8. 时间戳捕获策略（影响 §4.9/§12）

生产者恒捕获 CLOCK_REALTIME（轮转时间桶与文件归档命名依赖墙钟）；
仅当配置 `timestamp source = monotonic` 时额外捕获 CLOCK_MONOTONIC
（一次 clock_gettime ~20ns 的热路径成本）。`%time` 在 monotonic 模式
下输出相对 init 的秒.微秒（§12），time format 忽略。

## 9. TSan 构建下的测试豁免（影响 §13）

- `tests/integration/test_fork.c`：TSan 无法建模 fork 后再创建线程
  （reinit 路径在子进程重建消费者线程），TSan 构建下整个二进制不注册。
- `tests/unit/test_ring.c` 的 lockfree overwrite 用例（决策 4）：
  `HPU_RING_SKIP_OVERWRITE` 编译期宏在 TSan 下置 1。
其余并发测试（locked ring 全组合、无锁 discard 全组合、原子后端、
pipeline 热加载/fork 之外）TSan 零报告。

## 10. `json` 名字与时间扩展（影响 §12）

- 名为 `json` 的格式（内置或用户在 [formats] 重定义）承载 JSON 语义：
  对 %msg/%file/%category/%func 做 JSON 转义（跳过注入转义且不受开关
  影响），pid/tid 强制十进制。判断依据是格式名而非模板内容。
- 时间格式扩展：`%f` = 6 位微秒；`%F1..%F6` = n 位小数（截断微秒）。
  `%F` 不带数字保留 strftime 原义（ISO 日期）。时间渲染按秒缓存
  （prefix/suffix 两段 strftime 各一次/秒），分数部分逐条渲染。

## 11. INI=OFF 的裁剪面（影响 §4.8/§5）

`HPULOGC_ENABLE_INI=OFF` 时从构建排除 `src/conf/ini.c` 与
`src/conf/conf_file.c`；`hpulogc_init_from_file()` 保留符号并返回
`HPULOGC_ERR_CONFIG`（§4.8 裁剪 API 语义）。[throttle] 节在
THROTTLE=OFF、[async] 节在 ASYNC=OFF 下解析期整节静默跳过（§10.2）。
测试二进制相应按构建组合条件编译。

## 12. min 预设体积优化（影响 §5）

`HPULOGC_MIN_SIZE_OPT`（min 预设强制开启）：`-Os
-ffunction-sections -fdata-sections` + 链接 `-Wl,--gc-sections`。
注册表（约 20KB BSS）移出 `hpu_runtime_t` 独立定义，CATEGORY=OFF
构建由段 GC 裁剪。实测 .text 20,560 字节（详见 perf_report §5.4）。

## 13. 覆盖率豁免清单（影响 §13.1）

覆盖口径：`src/`（不含 `src/platform/`，平台 glue 豁免）。实测聚合
81.5%。低于 90% 的模块与豁免/原因：

| 模块 | 行覆盖 | 说明 |
|------|--------|------|
| output_console.c | 65.6% | 颜色路径需 isatty 终端（CI 管道重定向恒为 false，§4.7 行为本身即被测）；HPULOGC_ENABLE_COLOR=OFF 分支 |
| api.c | 58.3% | 仅 12 行（va 封装 + errno 保持），errno 路径被 test_lifecycle 覆盖但 gcov 归因于内联 |
| core.c | 75.6% | fork 三行为需 fork+TSan 豁免组合；错误分支（NO_MEM 等）依赖故障注入 |
| registry.c | 72.0% | 表满告警路径（64 类目注册压满才触发）与 CATEGORY=OFF 分支互斥 |
| hotreload.c | 76.1% | inotify 事件分类的"无关事件"分支依赖特定编辑器行为 |
| conf_file.c | 78.6% | HPULOGC_ENABLE_THROTTLE=OFF/ON 两分支仅覆盖当前构建（矩阵另一构建覆盖另一侧） |

环形缓冲 lockfree 实现的覆盖在 4 个 ring 测试二进制（tests 链接自有
副本）中统计，未计入上述 libhpulogc 口径；其 discard 路径全覆盖，
overwrite 路径受决策 9 限制。

## 14. 注册表 count 发布协议（影响 §4.9）

类目登记表为定长数组 + 原子 count：写入方在 mutex 下填充槽位后以
release 存储递增 count；读者 acquire 读取 count 后线性扫描。溢出
（>64 类目）按 `warned_hashes`（FNV-1a）对每个被拒类目仅告警一次。
注册条目缓存每级别命中的规则下标（`rule_for_level[7]`）与配置代数
（config_gen），热加载代数递增后惰性重算。

## 15. I/O 失败注入钩子（影响 §9/§13.2）

`output.h` 暴露 `int (*hpu_io_fail_hook)(int op, const char* path)`
（op: 0=open, 1=write, 2=fsync；默认 NULL 恒不失败）。仅测试使用
（test_diskfail），生产置 NULL；选择函数指针而非 LD_PRELOAD/链接封装
是为了保持平台层零间接开销。

## 16. 写失败后 written→dropped 的统计迁移（影响 §7.3/§9）

异步批量写：记录先入输出缓冲即计入 written；flush 失败时缓冲行数转入
累计 `lost`。`hpulogc_get_stats()` 报告 `written = 计数 - lost`、
`dropped += lost`，保证 accepted = written + dropped + overwritten +
throttled + 在途 的会计恒等式。

## 17. 热加载三阶段（影响 §10.5）

1. 解析新配置并校验（旧配置仅在 conf_lock 读锁下**只读**匹配 fd 复用
   计划——消除与日志读者的数据竞争，TSan 实测驱动出此重构）；
2. 写锁内：执行句柄搬迁（未变更 file output 复用 fd）+ 原子换指针 +
   代数递增；
3. 写锁外：关闭未被复用的旧输出并释放旧快照（写锁已排空所有旧读者）。
   任一前置失败整体回滚：仅关闭本轮新开的句柄，旧配置原封不动。
   reload 互斥串行化并发触发（watcher 线程与显式触发）。

## 18. `stats interval` 在同步构建下忽略（影响 §10.3）

统计输出依赖后台线程的周期检查；ASYNC=OFF 构建无消费者线程，init 时
告警一次并忽略（与 min 构建行为一致，§10.3 "min 构建下忽略"的推广）。

## 19. 清除 per-category 覆盖的哨兵值（影响 §7.3）

`hpulogc_set_level_for_category(cat, (hpulogc_level_t)-1)` 清除覆盖
恢复全局阈值；注册表内部以 `0xFF` 哨兵表示"未设置"。规范未定义清除
方式，此 API 约定记入 ABI 文档。

## 20. init 解析的 strict 语义（影响 §10.4）

init（`-1`）时由文件自身 strict init 治理（决策 6）；热加载按当前
生效值（§10.5 明文）。`[build]` 节任何不匹配仅警告，永不失败。

## 其他实现注记

- **consumer 唤醒协议**：无锁环无内建阻塞，采用 `consumer_waiting`
  原子标志 + kick 互斥量握手：消费者置位后空等（带超时），生产者
  提交后仅在标志置位时上锁广播——互斥量握手关闭丢唤醒窗口。
- **pad 记录不变式**：任何记录写入后，游标到边界的剩余空间 ≥
  `align8(sizeof(meta))`，否则该记录尾部扩展至边界；由此 pad 记录
  至少容纳完整头部，杜绝越界写（ASan 曾据此发现并修复）。
- **测试基础设施**：`hpu_test_util` 静态库提供自注册 TEST 宏运行器；
  ring 测试以"测试源码 + 指定 ring 实现 + 平台 OBJECT 库"按组合重链，
  与构建级 `HPULOGC_LOCKFREE` 选择解耦（曾因此发现 4 个二进制误链
  同一实现的问题）。

---

# Phase 2（Windows / MSVC）

## A. 重大基线偏差：src/core 模块缺失（必须首先阅读）

Phase 2 开始时发现 **Phase 1 基线未包含 `src/core/` 目录**：git 历史
（`72e8b20 feat: 完成 Phase 1` 提交及其之前）从未提交过
core_internal.h / core.c / api.c / pipeline.c / consumer.c / registry.c /
hotreload.c / signal_safe.c，工作区亦不存在；但 CMakeLists.txt、
docs/code_structure.md、决策 2/3/14/16/17 等均引用这些文件，测试
test_pipeline.c 直接 `#include "core/core_internal.h"` 并调用
`hpu_core_trigger_reload()`。

**处置**：本阶段依据以下材料重建了整个 core 模块（8 个文件）：
- rd_v0.2.md §4.9（八步管线）、§7.2/7.3/7.5（生命周期/控制/统计）、
  §9（fork/信号/统计语义）、§10.5（热加载）；
- docs/implementation_notes.md 的决策 2/3/14/16/17（consumer 唤醒
  协议、pad 不变式、count 发布协议、三阶段热加载等设计意图）；
- 既有测试的断言（test_lifecycle/test_pipeline/test_diskfail/
  test_fork/test_signal_safe 全部原样通过，未放宽任何断言）。
重建代码按 Phase 1 缺陷 A 的要求使用 `hpu_mutex_t` 等平台契约类型。
该偏差已同步至 code_structure.md 与最终报告；**此为如实记录，而非
隐瞒补写**。

## B. Phase 1 缺陷修复登记（§16.1 缺陷通道，逐条 diff 说明）

所有修复均为最小 diff；冻结文件 diff 全部在本清单内，无清单外改动。

| # | 位置 | 缺陷 | 修复 |
|---|------|------|------|
| A | src/core/core_internal.h、core.c、registry.c | 原任务书指出 Phase 1 core 直接使用 pthread 类型（core 随重建完成，直接按契约类型实现） | 全部使用 `hpu_mutex_t`/`hpu_atomic_*`；conf_lock 为普通 mutex（临界区极小，rwlock 无必要） |
| B | src/format/format.c（render_time） | 直接调用 `gmtime_r`/`localtime_r`（MSVC 无）；且 `newline_bytes()` 的 AUTO 分支假设 POSIX | 改调契约 `hpu_localtime(sec, &htm, use_utc)` 后搬运到 `struct tm`；AUTO 在 `_WIN32` 下输出 `\r\n`（§12） |
| C | src/conf/conf_model.c、src/output/rotate.c | `<unistd.h>`（getpid）MSVC 不存在 | `#ifdef _MSC_VER` 下 `_getpid()`，`hpu_getpid()` 宏收敛调用点 |
| D | include/hpulogc.h | `##__VA_ARGS__` 检测序列缺 `/Zc:preprocessor` 分支 | 按 Phase 2 默认不启用该开关：传统预处理器第 2 分支（`fmt, __VA_ARGS__` 依赖 MSVC 空参吞逗号）已验证可行（零可变参宏调用测试通过），头文件未改 |
| E | src/platform/posix/posix_time.c（hpu_localtime） | **新发现**：契约（hpu_time.h）规定入参为"epoch 秒"，实现却除以 1e9（按纳秒），所有按秒调用的路径（rotate.c 时间桶、format.c）落到 1970 年附近；Phase 1 测试只覆盖了内部自洽的 UTC 分支因此漏网 | 删除除法，按契约秒语义直传 `time_t`；Windows 侧按契约实现后跨平台行为一致 |
| F | src/atomic/hpulogc_atomic.h（hpu_cpu_relax） | **新发现**：x86 pause 提示仅依赖 `__x86_64__/__i386__`（GCC/Clang 宏），MSVC 下从未编译（空函数） | 增加 `_MSC_VER && _M_X64/_M_IX86 → _mm_pause()`、`_M_ARM64 → __yield()` 分支 |
| G | src/conf/conf_file.c | **新发现**：`<strings.h>`（strcasecmp）MSVC 不存在 | `_MSC_VER` 下 `#define strcasecmp _stricmp` |
| H | src/output/rotate.c（days_from_civil） | **新发现**：`(m + (m > 2 ? -3u : 9u))` 对 unsigned 取负，MSVC /W4 报 C4146（有符号性依赖实现行为） | 改为有符号 `int m_shift` 运算后再转 unsigned；算法数值不变 |
| I | src/platform/hpu_sync.h（WIN32 分支） | 占位 `void* impl` 无法承载值嵌入的 CRITICAL_SECTION（环形缓冲按值嵌入） | Phase 2 预定填充点：真实 `CRITICAL_SECTION`/`CONDITION_VARIABLE` 存储 + `WIN32_LEAN_AND_MEAN`/`NOMINMAX` 卫生宏 |
| J | CMakeLists.txt | 共享库导入库与静态库在 Windows 同名（hpulogc.lib）冲突（multiple rules generate） | Windows 下 DLL 输出名改为 `hpulogc_shared` |

**冻结文件 diff 总账**：`include/hpulogc.h`（未改，D 未启用）、
`src/atomic/hpulogc_atomic.h`（F）、`src/atomic/atomic_msvc.h`
（Windows 允许修改：MinGW `_ReadBarrier` 兼容宏、32 位 64 原子读/
写、CAS 未用序参数告警）、`src/conf/conf_file.c`（G）、
`src/conf/conf_model.c`（C）、`src/format/format.c`（B）、
`src/output/rotate.c`（C+H）、`src/platform/posix/posix_time.c`（E）、
`src/platform/hpu_sync.h`（I，win32 分支）、`CMakeLists.txt`
（平台分支 + J）。其余冻结目录（ring/core/conf/format/output 的
其余文件、platform/posix 其余文件）diff 为零——core 目录为重建而非
修改，已在上文 A 说明。

## C. Windows 平台实现要点

- **win32_sync**：CRITICAL_SECTION + CONDITION_VARIABLE；
  `hpu_cond_timedwait_ms` 直映射 `SleepConditionVariableCS`
  （0/1=超时/负 语义与 POSIX 版一致；CONDITION_VARIABLE 无需销毁）。
- **win32_thread**：`_beginthreadex`（CRT 初始化；CreateThread 会导致
  CRT 状态缺失）；join 后 CloseHandle；`hpu_atfork_register` 空实现
  返回 0（§9）。
- **win32_time**：单调 = QPC（频率缓存）；实时 =
  `GetSystemTimePreciseAsFileTime`（1601→1970 epoch，无符号 64 位
  100ns 换算）；civil 时间用 `gmtime_s`/`localtime_s`/`_mktime64`
  （DST 自动）。
- **win32_fs**：全部路径 UTF-8→UTF-16（CP_UTF8）后调 W 版 API。
  追加写 = `CreateFileW(OPEN_ALWAYS, FILE_GENERIC_WRITE)` +
  `SetFilePointer(FILE_END)`：不用 FILE_APPEND_DATA 是因为它与
  FlushFileBuffers（fsync=_commit 等价）的组合受句柄访问模式约束，
  且库已在每 output 层串行化写者，显式 seek-to-end 语义等价且 fsync
  可用。写全循环 WriteFile；`MoveFileExW(REPLACE_EXISTING)`（rename，
  POSIX rename 语义）；目录扫描 FindFirstFileW/FindNextFileW（排除
  "."/".."，UTF-16→UTF-8 堆串返回）；fchmod 返回 0；symlink 恒 -1。
- **权限/软链一次性告警**（§4.7）：win32_fs.c 内 CAS 哨兵
  （`hpu_at_cas_u32`）实现每进程至多一条 stderr 警告——文件权限
  （open_append 时 mode≠0）、目录权限（mkdir 时 mode≠0）各一条；
  symlink latest 的告警在输出打开路径（Phase 1 既有逻辑：打开失败
  容忍 + 轮转时一次），Windows 下 hpu_fs_symlink 恒失败故不重复。
- **VT 控制台颜色**（§16.2）：`hpu_fs_isatty(fd 1/2)` 首次调用时
  `_setmode(1/2, _O_BINARY)`（见决策 23）+ `SetConsoleMode`
  启用 `ENABLE_VIRTUAL_TERMINAL_PROCESSING`；任一步失败返回 0，
  颜色层自动降级（重定向管道/文件天然无 GetConsoleMode）。
- **win32_path**：'/' 与 '\\' 双分隔符；normalize 输出统一 '/'
  并折叠 ASCII 大小写（供 §10.4 重复路径检测）；盘符前缀（X:）识别
  为绝对路径。
- **win32_watcher**：恒轮询后端。`GetFileAttributesExW` 的
  LastWriteTime（100ns）+ 文件大小做基线；`hpu_watcher_wait` 按
  ≤200ms 步进轮询至超时；文件消失相对既有基线视为变更。
- **win32_tid**：GetCurrentThreadId（读 TEB，无系统调用，无需缓存）。
- **win32_tls**：**FlsAlloc 族**（非 TlsAlloc）：FLS 是 Windows 唯一
  支持线程退出析构回调的槽 API，per-thread 渲染缓冲的释放依赖它；
  析构经固定 8 槽 trampoline 转发用户 dtor。
- **win32_signal**：`hpu_signal_install_hup` 恒 -1、
  `hpu_signal_take_hup` 恒 0（契约规定）；init 路径告警一次后继续
  （仅轮询热加载）。

## D. Windows 特有行为决策

- **决策 21（§7.3 会计）**：同步路径与消费者线程共用的
  `hpu_pipeline_process()` 在记录路由到零输出（或全部写出失败后
  重开仍失败）时计入 `st_retired_lost`（= dropped），保证
  accepted = written + dropped + overwritten + throttled + 在途
  恒等式在 sync/async 两种模式下闭合。
- **决策 22（§7.3 flush 语义）**：同步构建的 `hpulogc_flush()` 为
  best-effort（"尽量写出"）：写失败已按 §9 处理（重开一次→计入
  dropped + 限频告警），不再通过返回码上报——与异步构建消费者吸收
  错误的行为对齐。这是 Phase 2 首次真正在 ASYNC=OFF 构建下运行
  test_diskfail（Phase 1 矩阵同样会暴露此问题）发现的语义缺口。
- **决策 23（§12 二进制模式）**：`_setmode(1/2, _O_BINARY)` 在
  首次 isatty 检查时无条件执行（不等 isatty 结果）：CRT 文本模式
  对**任何**非二进制 stdio 流做 \n→\r\n 翻译，会把库渲染好的 \r\n
  变成 \r\r\n（实测发现），强制 LF 也会被破坏。库自身负责换行
  字节，CRT 不得二次翻译。
- **决策 24（§13.2 JSON 校验）**：Windows CI 镜像 python 可用性
  不定，`json_output_validity` 先尝试外部 python 校验，不可用或
  失败时回退内置结构校验（逐行引号感知的花括号配平 + 字符串未闭合
  检测）；两者都失败才判 FAIL。
- **MSVC 自注册测试**：`test_util.h` 的 TEST 宏在 MSVC 下用
  `#pragma section(".CRT$XCU")` + `__declspec(allocate)` 用户初始化
  段注册（GCC/Clang 保留 `__attribute__((constructor))`）；每用例
  独立指针符号，/OPT:REF 段 GC 下已验证存活。
- **测试线程 shim**：`tests/portability.h`（tests/ 允许域）封装
  `_beginthreadex`（pthread 形态入口 trampoline 适配）、
  CONDITION_VARIABLE 计数屏障（替代 pthread_barrier）、Sleep、
  _getpid、%TEMP% 临时目录、DeleteFile/_rmdir/递归删除、_popen、
  QPC 计时（hpu_test_now_ns）。测试源码的平台差异集中于此。
- **test_fork 不参与 Windows 构建**（§9）：无 fork；CMake `if(NOT
  WIN32 ...)` 排除，理由记录于此。
- **test_signal_safe 信号适配**：MSVC CRT 无 SIGUSR1，handler 改挂
  SIGABRT（raise(SIGABRT) 走 CRT 信号路径，回调内仅调
  async-signal-safe API，语义等价）；POSIX 保持 SIGUSR1 + sigaction。
- **examples 路径**：/tmp/... 改为相对路径（Windows 无 /tmp；相对
  路径双平台可写）。
- **`/wd4210`**：frozen `src/output/output.c` 在函数块内声明 extern
  函数（合法 C99，MSVC 风格扩展告警 C4210）——冻结文件不改，编译
  选项豁免并记录。
- **`_CRT_SECURE_NO_WARNINGS`**：frozen 代码的 fopen/strerror 等
  CRT "安全"弃用告警统一关闭（这些调用的边界安全已由 §9 字符串
  约束保证；切换 _s 版本将违反零修改边界）。
- **min 预设 MSVC 体积口径**：`/Os /Gy /Gw /Zc:inline` +
  `/OPT:REF /OPT:ICF`（对应 GCC 的 -Os + 函数段 GC）。
- **`hpu_core_trigger_reload` 测试入口**：重建 core 时保留 Phase 1
  测试已引用的内部符号名与签名（`int hpu_core_trigger_reload(void)`，
  返回 0 成功/-1 回滚），test_pipeline 无需修改即通过。

## E. Phase 2 测试与验证结果索引

- MSVC（VS2026, cl 19.50, /W4 /utf-8 零警告）+ Ninja 全量构建。
- CTest：full 预设 17/17（含 4 示例）；min 12/12；sync_thread 13/13；
  async_single 13/13（详见 perf_report_windows.md）。
- 功能矩阵：{C99, C11} × {有锁, 无锁} × {SPSC, MPSC} = 8 组合 +
  4 预设，`scripts/run_matrix.ps1` 全绿（含每组合零告警门禁）。
- ASan（/fsanitize=address）：ctest 17/17 零报告。
- /analyze 静态分析：0 警告（库目标全量编译）。
- 行为核查：tests/check_windows_behavior.c 7/7（实时钟对表、QPC
  单调、UTF-8 中文目录建/写/轮转、权限/软链忽略+一次性告警、
  auto 换行 \r\n、lf 强制、lockfree-MPSC overwrite fail-fast）。
- 零可变参宏（MSVC 传统预处理器）：HPULOGC_INFO("cat", "no args")
  正确展开（独立验证程序）。
- MinGW-w64 × x64：见 perf_report_windows.md §6（工具可用，已验证）。

---

# Phase 3（macOS / Apple Clang）

## A. 实施环境说明（诚实记录）

本阶段无本地 macOS 机器：代码在 Windows/WSL 环境编写，macOS 侧的
验证全部通过 GitHub Actions（`macos-latest`，Apple Silicon，Xcode
Clang）承担（见 .github/workflows/ci.yml 的 macOS 矩阵，指令 4）。
凡涉及 macOS 运行时行为的结论均标注"经 CI 验证"或"待 CI 验证"，
本文件与报告不以本地推测定论。

## B. Phase 1 缺陷修复登记（缺陷通道，冻结文件最小 diff）

| # | 位置 | 缺陷 | 修复 |
|---|------|------|------|
| M | src/platform/posix/posix_watcher_poll.c | **新发现**：读取 mtime 使用 `st_mtim`——Darwin 的 `struct stat` 字段名为 `st_mtimespec`，共享轮询后端在 macOS 上编译失败 | 新增 `stat_mtime_ns()` 辅助函数，`#ifdef __APPLE__` 选择字段；Linux 路径数值行为不变 |
| N | src/core/consumer.c（hpu_consumer_flush） | **新发现**（CI 暴露）：macOS CI runner 多 job 并行拥塞下，5s 硬编码 flush 等待上限不足以让正常管线完成排空——`hpulogc_flush()` 误判失败 | 将等待上限改为跟随配置的 `shutdown_timeout_ms`（默认 5000），CI 重跑后稳定通过 |

冻结文件 diff 总账（Phase 3）：`posix_watcher_poll.c`（M，冻结文件最小 diff）与 `core/consumer.c`（N，core 为 Phase 2 重建文件，非全新基线）；其余改动位于 darwin 专属目录、tests/（允许域）、CMake 与 ci.yml。

## C. macOS 平台实现要点

- **darwin_sync.c**：macOS 的 `pthread_condattr_setclock()` 仅接受
  `CLOCK_REALTIME`，POSIX 共享层的 MONOTONIC 绝对期限方案在 macOS
  上 `hpu_cond_init` 即失败（运行时硬阻塞）。替代：condvar 以默认
  （REALTIME）属性初始化，timedwait 用
  `pthread_cond_timedwait_relative_np`——相对期限不依赖任何时钟，
  免受墙钟跳变影响，与 Linux MONOTONIC condattr 的保证等价。
- **darwin_tid.c**：`pthread_threadid_np(NULL, &tid)` 返回系统级
  线程 ID（契约 hpu_tid.h 指定；POSIX 层的 pthread_self 指针强转
  fallback 不是系统级 ID）。macOS 10.6+ 可用，无需缓存。
- **watcher**：按 §16.3 "kqueue 或轮询回退"的选择，Phase 3 使用
  POSIX 轮询后端；后续增强工单实现 kqueue（EVFILT_VNODE）后端
  （darwin_watcher.c：文件 vnode 写/删/改名 + 父目录事件经 stat
  基线确认，删除/改名后下一次等待自动重挂新 vnode，kqueue 不可用
  时透明回退轮询），原 posix_watcher.c 仅保留给无原生机制的平台。
- **原子后端**：无需新代码。C11 走 `<stdatomic.h>`（Apple Clang
  探测通过），C99 走 `__atomic_*`；`atomic_gcc.h`/`atomic_stdatomic.h`
  直接复用，OSAtomic* 按规范禁用未使用。
- **其余契约**（thread/time/fs/path/tls/signal）直接复用 POSIX
  共享层；clock_gettime 需 macOS 10.12+（README 注明最低版本）。
- **min 预设链接器**：Apple 链接器无 `--gc-sections`，改
  `-Wl,-dead_strip`（等价的段回收）。

## D. 测试基建适配（tests/ 允许域）

- **portability.h**：Apple 无 `pthread_barrier_t`（POSIX 可选项），
  为 `__APPLE__` 补 mutex+cond 计数屏障（与 Windows 分支同方案）；
  Linux 的 `pthread_barrier_t` 路径不变。
- **bench_memory.c**：macOS 无 /proc 亦无 mallinfo2，新增
  `task_info(MACH_TASK_BASIC_INFO)` 的 resident_size 口径分支。
- **test_fork**：macOS 有 fork，测试参与运行（TSan 构建按决策 9
  继续豁免）。

## E. Phase 3 验证结果索引

**本地（无 macOS 机器）**：
- Linux 全矩阵（WSL ubuntu 26.04：GCC 15.2 + Clang 22.1，
  {99,11}×{锁,无锁}×{SPSC,MPSC} 16 组合）**16/16 全绿**；
  四预设（full/min/sync_thread/async_single）**4/4 全绿**；
- 同组合（c11 有锁 SPSC）30 次重复压测 0 失败；
- MSVC `/W4` 零告警 + ctest **17/17**（Windows 无回归）。

**macOS（GitHub Actions，macos-latest = Apple Silicon + Apple Clang）**：
PR #2（`phase3/macos`）三轮迭代后 **run 35485896218 结论 success**，
全部必要 job 通过：

| job 组 | 数量 | 结果 |
|--------|------|------|
| linux-smoke | 4 | 全绿（POSIX 共享层改动回归确认） |
| macos-matrix | 8 | 全绿（{C99,C11}×{有锁,无锁}×{SPSC,MPSC}；C99 走 `__atomic_*`、C11 走 `<stdatomic.h>`，Apple Clang 双标准均实测通过） |
| macos-presets | 4 | 全绿（full/min/sync_thread/async_single） |
| macos-asan | 1 | 全绿（`-fsanitize=address` 全量 ctest，含 LeakSanitizer，零报告） |
| macos-arch | 2 | 全绿（x86_64 二进制经 Rosetta 运行：无锁 SPSC + 有锁 MPSC） |
| macos-tsan | — | 按设计仅每周一定时（21:00 UTC）与手动 dispatch 触发（本次计划跳过，配置就绪） |

迭代记录（CI 暴露的真实缺陷，均按缺陷通道登记）：
1. 第一轮 `macos preset min` 失败 —— min 预设 INI=OFF，
   `example_custom_format` 依赖 `init_from_file` 必然失败；
   对齐 Phase 1/2 矩阵脚本口径（预设不含 examples）后通过。
2. 第二轮 `macos c11 OFF SPSC` 失败（首轮通过，flaky）—— 慢
   runner 并行拥塞下 5s flush 上限过短（缺陷 N）；修复后稳定。
3. 第三轮全绿。

**未编入报告项（诚实记录）**：
- **perf_report_macos.md 未编制**：共享 CI runner（虚拟化、多租户、
  无绑核）不是有效性能测量环境，产出数值不具可比性；按"不为凑
  报告而编造/采集无效数据"原则，macOS 性能数据留待具备真机时
  按 Phase 1/2 同口径采集（方法学已由 bench_log/bench_memory 的
  darwin 分支就绪）。
- **Dr. Memory 未运行**：本机（Windows）未安装；MSVC ASan 已
  覆盖 Windows 内存检查（Phase 2 报告 §记录）；macOS 侧由
  CI 的 ASan（含 LSan）覆盖。
- **24h 压测**：发布前门禁，非本次范围（同 Phase 1/2 记录）。
- **遗留（可选增强）**：universal binary（x86_64+arm64 单文件）；
  非 §16.3 DoD 要求。kqueue watcher 后端已实现（见 watcher 条目）。

---

# Phase 4（MPMC 跨平台扩展，规范依据 docs/rd_v0.3.md）

## A. 决策登记（新增决策 25–27）

| # | 决策 | 影响章节（rd_v0.3） |
|---|------|---------------------|
| 25 | MPMC 采用单共享环 + N 对称消费者：每条记录恰好被一个消费者认领处理；处理阶段沿用全局 conf_lock（v1 并行化出队；每消费者独立输出缓冲列为后续优化）。**MPMC 不保证记录间全局输出顺序**（文档明示，调用方需顺序保证时用 SPSC/MPSC） | §1.1 |
| 26 | 消费者数量来自 `[async] consumer threads`（默认 1，范围 1–16，init-only、热加载忽略+警告）；非 MPMC 构建 > 1 → `HPULOGC_ERR_CONFIG`（沿用"值合法但构建不支持"fail-fast 规则）；默认 1 保证配置文件跨构建可移植 | §1.3 |
| 27 | 无锁 MPMC 出队采用 **commit 字租约认领**：消费者先 CAS 记录头 commit 字（`pos+1 → pos+2`）获得独占读租约，**租约持有期间拷贝**（拷贝跳过 commit 字本身），完成后推进 `deq`。租约期内生产者不可能复用该内存（deq 未越过），因此无锁 MPMC discard 路径**物理无数据竞争、TSan 干净**——与无锁 overwrite 的 seqlock 固有物理竞争（决策 4、TSan 跳测）本质不同。SPSC/MPSC 保持原单消费者路径零开销 | §1.2/§5 |

补充要点：

- **消费者唤醒计数器**：无锁环 `consumer_waiting` 由单标志改为计数
  （fetch_add/fetch_sub），lf_kick 条件 `!= 0` 不变；多消费者同 Park/
  同广播语义保持，互斥量握手仍关闭丢唤醒窗口。
- **flush 握手**：`flush_done` 单计数改为每消费者 ack 数组；flush 等
  待环空且全部 ack ≥ seq（上限仍随 shutdown timeout）。
- **shutdown**：`consumer_alive` 改为存活计数；stop 置共享 exit 标志
  → kick → 有界等计数归零 → join 全部 → 释放数组。
- **fork**：atfork 子进程处理器重置消费者簿记（线程数组/ack 数组置
  NULL、存活计数清零），惰性重建按当前配置重新拉起 N 个消费者（与
  既有 ring discard 决策 3 同风格，继承分配被放弃）。
- **staging 内存**：每消费者按环容量独立分配（N × buffer size），
  缩减为按最大记录分配列为后续优化（rd_v0.3 §5）。
- **统计口径**：`written` 等计数本为原子累加，多消费者并发递增无需
  改动；stats 周期报告仅由 0 号消费者发出（避免 N 倍输出）。

## B. 测试扩展

- `tests/unit/test_ring.c`：`HPU_RING_TEST_CONC=2`（MPMC）组合；
  新增 `ring_stress_mpmc_no_loss`（4P×2000×4C，exactly-once 矩阵逐格
  校验）、`ring_stress_mpmc_discard_pressure`（8KB 环记账恒等式）、
  `ring_mpmc_close_drain`（关环后启动消费者协同排空）、
  `ring_mpmc_wait_and_overwrite`（有锁 MPMC 的 wait/overwrite 全策略）、
  `ring_lockfree_mpmc_overwrite_maps_to_discard`（防御性降级）。
  exactly-once 矩阵用原子单元 fetch_add 标记，重复投递在测试层即为
  显式断言失败。
- `tests/integration/test_mpmc.c`（仅 MPMC 构建注册）：端到端
  4 消费者、flush 先于 shutdown 可见性、统计恒等式、构建信息字符串、
  配置范围校验。
- `tests/unit/test_lifecycle.c`：`consumer_threads_gating` 全构建
  门禁（范围校验 + 非 MPMC 构建 fail-fast / MPMC 构建成功双断言）。
- 环测试组合 4→6（locked/lockfree × SPSC/MPSC/MPMC）。

## C. 验证结果索引（本地 Linux，全量）

| 项 | 结果 |
|----|------|
| `scripts/run_matrix.sh`（扩展后 24 组合：{gcc,clang}×{99,11}×{有锁,无锁}×{SPSC,MPSC,MPMC}） | 24/24 全绿 |
| MPMC 构建（locked 默认 + lockfree）完整 ctest | 19/19 全绿（18 + test_mpmc） |
| ASan+UBSan（默认构建） | 18/18 全绿，零报告 |
| TSan（lockfree MPMC，RelWithDebInfo） | test_ring_lockfree_mpmc 连续 3 轮零竞争报告；locked MPMC 同样零报告 |
| Windows / macOS | 按 Phase 2/3 同惯例完全由 GitHub Actions 承担（见 ci.yml / win_msvc.yml 的 MPMC 组合） |

## D. 预存缺陷登记（本阶段发现，非 MPMC 引入；缺陷通道，待后续修复）

TSan（Linux，lockfree 构建）下 `test_pipeline` 报告两处数据竞争，
已用干净 worktree 在 main（commit 2910402）复现，与 MPMC 改动无关：

| # | 位置 | 竞争 | 说明 |
|---|------|------|------|
| P-1 | `src/core/core.c` / `hpu_runtime_t.state`（**已修复**，独立工单 PR） | watcher 线程在 `hpu_core_trigger_reload` 入口读取 `g_rt.state`（普通 int） vs init/shutdown 线程的普通写 | **修复**：`state` 字段原子化（`hpu_atomic_u32`），全仓访问点改经 `hpu_rt_state_load/store` 内联辅助（acquire/release）；`state` 实际被生产者/消费者/watcher 线程无锁轮询，原"普通 int read-mostly"约定对 TSan 不可建模，原子化后语义不变且 Happens-before 显式化 |
| P-2 | `src/core/hotreload.c` Phase 1 解析路径（**已修复**，独立工单 PR） | `hpu_conf_finalize_reload(fresh, g_rt.conf, ...)` 在**未持 conf_lock** 的情况下读取 `g_rt.conf` 指针，与换代写（持锁）构成指针大小竞争 | **修复（三件套）**：① reload 入口在 conf_lock 内快照 `g_rt.conf` 到局部 `live` 并复核 state（Phase 1 全程改用局部指针）；② shutdown 的 conf 释放改持 conf_lock；③ `hpu_core_stop` 在 join watcher 后、拆管线前有界等待 `hpu_hotreload_busy()` 清零（上限 6 s，覆盖显式触发自身的 5 s 槽位期限），显式触发与 shutdown 并发时不再可能读到已释放快照（原缺陷实为 UAF 级，非仅 TSan 可见性） |
| P-3 | `src/core/core.c` init 路径（**已在本阶段修复**） | `fork behavior`（spec 9，配置文件专属键）从未从配置快照复制到 `g_rt.fork_behavior`：全代码仅代码配置路径硬编码 `HPU_FORK_REINIT`，文件配置的 `disable` 实际按 `reinit` 执行（子进程重建+重启消费者）。`fork_disable_child_drops` 一直靠"子进程 `_exit` 前输出缓冲未被刷出"的时序运气通过；macOS runner 的调度差异使该运气失效（`c11 OFF MPSC`、`x86_64 lockfree-SPSC` 两组合首轮 CI 失败） | 修复：`init_common` 统一 `g_rt.fork_behavior = c->fork_behavior`（三条 init 路径共用；代码配置经 defaults 仍为 reinit，行为不变），删除冗余硬编码；本地两个构建 fork 测试确定性通过 |

影响：TSan 定时任务中 `test_pipeline` 曾以退出码 66 失败（TSan 惯例
exitcode=66）。该失败在 main 上即存在；MPMC 组合加入 TSan 矩阵不改
变其状态。MPMC 自身新增路径（ring 租约认领、多消费者 core/flush/
shutdown）经上述 C 节验证为 TSan 干净。

**后续（独立工单，已合并）**：P-1/P-2 修复后 `test_pipeline` 在 TSan
下（默认构建与 lockfree-MPMC 组合）连续多次运行零竞争报告，TSan
nightly 不再有预期内失败。同工单顺带清理了误提交的 `[DBG]` stderr
调试打印（8b91072 引入）。

| P-5 | `docs/rd_v0.6.md` §4.7.3 措辞 vs `src/output/output.c` 实现（**文档级**） | 规范称 `enabled=false` 的实例"**不打开**、不参与路由"，实现为"实例与资源仍建立、仅 deliver 入口门禁（`output.c` `!b->enabled` 直接返回）" | 实现语义恰是"热重载翻转即时生效、无需重开资源"（§4.7.3 同条）的基础；`examples/conf_eg.ini` 注释按实现语义表述，规范措辞待下次规范修订时统一（`examples/conf_eg.ini` 编制时发现） |

| P-4 | `src/ring/ringbuf_lockfree.c` MPMC 过期游标重同步读（**部分已修**） | 消费者游标过期（deq 已前进且环回绕）时，lf_wait_commit 的 commit 读取与生产者对同一物理位置的重写构成 TSan 可见竞争；若负载字节伪造 commit 匹配，租约 CAS 会以垃圾 total_len 推进 deq（真实正确性风险，非仅良性） | **修复（本阶段）**：lf_wait_commit 将 deq==d 新鲜度检查前置到 commit 读取之前；MPMC 认领 CAS 前再次校验 deq==d。deq==d 时生产者不可能写该物理槽位（空间检查 pos+len ≤ deq+cap 对同槽位重写恒假），且 deq 跳过 d 必须先取得 d 的租约（互斥），故「新鲜度检查 + 认领 CAS」对重用窗口封闭。修复后 correctness 封闭；残余的**良性重同步读**（检查与读取之间的极端 TOCTOU 窗口，值必被门禁丢弃）TSan 仍无法建模——`ring_stress_mpmc_discard_pressure` 在 TSan 下自跳过（同决策 4/9 先例），其余 MPMC 用例 TSan 零竞争 |

另：首轮 PR CI 的 macOS 两组合失败（`fork_disable_child_drops`）即为
上表 P-3 暴露，修复后复跑通过（详见 §E）。


---

# Phase 5（多 Sink 体系，规范依据 docs/rd_v0.6.md §4.7/§4.10/§4.11）

## A. 验证结果索引（本地 Linux）

| 项 | 结果 |
|----|------|
| `scripts/run_matrix.sh quick`（24 组合：{gcc,clang}×{99,11}×{有锁,无锁}×{SPSC,MPSC,MPMC}） | 24/24 全绿 |
| 默认构建完整 ctest（22 项，含 4 个新 sink 测试二进制） | 22/22 全绿 |
| MPMC 构建完整 ctest（23 项，含 test_mpmc + test_sink_async MPMC 用例） | 23/23 全绿 |
| ASan+UBSan（含 test_fields 长键越界、test_sinks kv 悬垂捕获） | 22/22 全绿 |
| TSan（lockfree MPMC） | 仅 test_pipeline 预存竞争（§D P-1/P-2）；ring 过期游标真实漏洞（P-4）已修复；MPMC 压测良性重同步读按决策 9 先例自跳过 |

## B. 新增缺陷登记（Phase 5 开发中 ASan/TSan/clang 捕获，均已修复）

| # | 缺陷 | 捕获者 | 修复 |
|---|------|--------|------|
| D-1 | BOOL 字段解包仅拷 1 字节进 4 字节 int，高位垃圾（非确定值） | clang 矩阵（gcc 静默） | unpack 拷贝前清零联合体 |
| D-2 | outputs 数组 realloc 后 kv 池指针悬垂 → strlen 越界读 | ASan heap-use-after-free | kv 改偏移量存储（realloc 安全） |
| D-3 | test_fields longkey 未 NUL 结尾（测试 bug） | ASan stack-buffer-overflow | 测试数据补 NUL |
| P-4 | MPMC 过期游标认领 CAS 可落入重写后的槽位（负载伪造 commit 时真实腐蚀） | TSan | deq 新鲜度门禁（检查+CAS 对重用窗口封闭） |

## C. 遗留与说明

- `hpulogc_build_info_t` 未新增 sinks 字段（无规范性要求，避免 ABI 扰动；
  如需可按 append-only 规则追加）。
- `hpu_output_write_direct`（v0.2 死代码）已随 vtable 化删除。

---

# 网络型 sink 第一期（TCP/UDP，rd_v0.6 v0.6.2 §4.10.8）实现自由度登记

> 任务书 §五.1 授权"实现自由度自定后登记"。以下各项均不触及规范性语义
> （规范已定案的部分照办），仅登记规范未钉死的实现选择与理由。

| # | 自由度 | 决定 | 理由与代价 |
|---|--------|------|-----------|
| N-1 | start fail-fast vs 懒连接 | **保持 fail-fast**（首连/解析失败 → `HPULOGC_ERR_IO`） | 配置错误早暴露（与 rollingfile 打开语义一致）；"远端暂不可达"场景由运行期重连机制覆盖。代价：远端暂不可达时 init 失败，调用方需自行重试 init 或先建好远端 |
| N-2 | UDP 截断 vs 分片 | **截断至 mtu 后仍投递**，该事件计 `failed` 不计 `written` | 分片放大丢包（规范已定不做）；"仍投递"使接收方收到可解析的截断行，记账如实反映降级。替代方案（直接丢弃）使接收方少一条完整可解析记录，且无法区分"超长"与"丢弃"，弃 |
| N-3 | 事件借用与深拷贝边界 | 发送全部在回调内同步完成，**无跨回调保留**，故除 TCP 合并批缓冲外不做额外深拷贝 | §4.10.4 要求"跨出回调保留必须深拷贝"——本实现没有跨回调保留场景（异步路径由第二级队列已完成深拷贝；重连采用丢弃语义不缓存事件）。TCP emit_batch 的 64KB 合并缓冲即 priv 预分配缓冲（稳态零 malloc） |
| N-4 | TCP emit_batch 记账口径 | 合并行帧写入；**发送出错时已离开进程的行计 written、仍在合并缓冲内的行计 failed**（返回值表达部分成功） | 比整批失败口径（返回 -1）更精确；代价：断线瞬间接收端可能出现撕裂行（规范已明示接收方丢弃无终止符残行） |
| N-5 | connect 无超时参数 | v1 使用阻塞 connect，不引入 `connect timeout` 配置键 | 避免规范外新增键；阻塞面由"推荐 async=on"覆盖（start 仍在调用方线程，文档已明示）。若后续需要，键加入属规范修订 |
| N-6 | 重连不重新解析 DNS | 重连沿用 start 时解析的地址（§4.10.8 已写入规范） | 回调内 getaddrinfo 会阻塞且动态分配（违反 §4.10.4）；代价：远端 IP 长期变更需热重载重建 |
| N-7 | UDP EAGAIN 判定路径 | socket 恒 `O_NONBLOCK`（hpu_net_dgram_open 契约保证），EAGAIN → 计 failed | 使"EAGAIN 丢弃不阻塞"语义在 emit 路径可观测，而非依赖内核缓冲恰好未满 |
| N-8 | 测试的 fail-fast 路径选择 | `test_tcp_start_refused_fails_fast` 用**不可解析域名**（.invalid）而非 connection-refused | WSL2 localhost 中继使 loopback 已关闭端口的 connect 仍"成功"（后随 RST），refused 前提在该环境不成立；解析失败路径跨环境确定。CI Linux/macOS 上两者均可靠，取更稳健者 |
