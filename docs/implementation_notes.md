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
