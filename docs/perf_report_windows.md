# hpulogc 性能测试报告（Phase 2 / Windows 原生）

- 日期：2026-09-20
- 库版本：0.1.0（Phase 2）
- 测试人：自动化任务执行（本报告为实测原始数据，无估算值）
- 对照报告：docs/perf_report.md（Phase 1 / Linux / WSL2）

## 1. 环境说明

| 项 | 值 |
|----|----|
| CPU | Intel(R) Core(TM) Ultra 9 275HX（**Windows 原生**，非虚拟化） |
| 内存 | 64 GB（63 GiB 可见） |
| 操作系统 | Microsoft Windows 11 专业工作站版（10.0.26200，SDK 10.0.26100） |
| MSVC | 19.50.35723（VS2026，工具集 14.50.35717） |
| CMake / Ninja | CMake 4.3.0；Ninja（VS 自带 1.13.x） |
| MinGW-w64（对照） | GCC 16.1.0（x86_64-posix-seh，D:\dvp\mingw64） |
| 基准程序 | `tests/bench/bench_log.c`、`tests/bench/bench_memory.c`（随构建系统提供） |

基准构建配置：MSVC `/O2`（CMake Release 默认），库与基准同配置；计时用
QPC（`QueryPerformanceCounter`，经 tests/portability.h 的
`hpu_test_now_ns()`）；预热 10,000 条后正式测量；延迟场景 P50/P99/P999
（逐条采样 200,000 条/生产者，QPC 采样开销计入数值）；吞吐固定 5 秒
窗口 ×2 轮；延迟 3 轮取中位；起跑用 CONDITION_VARIABLE 计数屏障
（tests/portability.h 的 `hpu_test_barrier_t`）。

## 2. 方法学（Windows 适配口径说明）

1. **延迟（逐条采样）**：每次 `hpulogc_log()` 前后取 QPC，差值 ns；
   多生产者场景屏障同步起跑。**注意**：每次采样含 2 次 QPC 调用
   （实测单次 QPC ~20 ns，比 Linux vDSO clock_gettime 略高），计入
   下述数值。
2. **延迟（摊销）**：单次时钟对包裹 200,000 条循环，总时差/条数——
   无逐条采样开销，反映真实平均单条成本。
3. **吞吐**：固定 5 秒持续写入（64B/条），以 `hpulogc_get_stats()`
   的 accepted 差值计算；输出目标 `NUL`（Windows 的 /dev/null 等价
   设备），消费者批量同步写。
4. **基线内存**：`GetProcessMemoryInfo()` 两个口径：
   - **Private commit 差值**（`PROCESS_MEMORY_COUNTERS_EX.PrivateUsage`，
     对应进程私有提交字节）——**本报告的达标口径**，与 Phase 1 的
     mallinfo2 堆在用口径同类（库自身分配）；
   - **WorkingSet 差值**（`WorkingSetSize`）仅作参考并列：Windows 的
     工作集包含 CRT/系统 DLL 首次触碰的页面（本机实测 ~100 KB），
     受加载器/CRT 启动行为支配，与 Phase 1 报告中 statm 口径受
     glibc arena 支配同理，不作为达标依据。
   - **mallinfo2 无 Windows 等价**：未复刻堆在用口径；私有提交差值
     承担同等角色（预触 1 MB 再释放，排除分配器 arena 初始化成本）。
5. **min 体积**：`HPULOGC_BUILD_PRESET=min`（`/Os /Gy /Gw /Zc:inline`
   + 链接 `/OPT:REF /OPT:ICF`）链接 examples/minimal.c，`dumpbin
   /headers` 读 `.text` 的 virtual size。与 Phase 1 的
   `-Os -ffunction-sections --gc-sections` + `size -A` 口径对应
   （均为可执行段/代码段净尺寸）。

## 3. 结果总表（目标值取 rd_v0.2.md §8）

| 指标 | 目标 | 实测（中位/最优） | 结论 |
|------|------|-------------------|------|
| 单条延迟（无竞争，SPSC+无锁+64B） | < 100 ns | 逐条采样 P50 = **300 ns**（3 轮 200/300/300）；摊销均值 = **155 ns**（155/113/155） | **未达标**（原因见 §5.1，与 Phase 1 同构） |
| P99 延迟（MPSC+无锁+8 生产者） | < 1 μs | **1.6 μs**（1.5/1.6/1.6 μs） | **未达标**（边缘超 60%，见 §5.2） |
| P99 延迟（MPSC+有锁+8 生产者） | < 10 μs | **23.3 μs**（23.3/21.6/30.4 μs） | **未达标**（原因见 §5.3） |
| 吞吐峰值（64B，异步批量） | > 500,000 logs/sec | 最优 **5,157,200 logs/sec**（lockfree MPSC）；locked MPSC 4.1M | **达标**（10.3×） |
| 基线内存（min，不含缓冲区） | < 32 KB | 私有提交差值 **8 KB** | **达标** |
| min 体积（.text） | ≤ 32 KB | **20,428 B**（4FCC hex） | **达标**（与 Phase 1 的 20,560 B 同量级） |

## 4. 各构建组合延迟明细（64B 消息，200k 采样/生产者，3 轮中位）

| 构建 | SPSC P50 | SPSC P99 | 8P P50 | 8P P99 | 8P P999 | 摊销均值 |
|------|----------|----------|--------|--------|---------|----------|
| lockfree+SPSC | 300 ns | 400 ns | —（SPSC 构建无 MPSC 路径） | — | — | 155 ns |
| lockfree+MPSC | 200–300 ns | 400 ns | 500 ns | 1,600 ns | 2.9–13.4 μs | 152 ns |
| locked+MPSC | 300 ns | 500 ns | 1,900 ns | 23,300 ns | 55.2 μs | 232 ns |

吞吐（5 秒窗口，2 轮，64B，异步批量，输出 NUL）：

| 构建 | 第 1 轮 | 第 2 轮 |
|------|---------|---------|
| lockfree+SPSC | 4,042,800 logs/sec | **5,028,400** |
| lockfree+MPSC | 4,678,800 | **5,157,200** |
| locked+MPSC | 3,109,600 | **4,106,200** |

内存（min 预设，buffer=4KB 未触碰不计入）：

| 口径 | 数值 |
|------|------|
| Private commit 差值 | **8 KB**（3 轮一致） |
| WorkingSet 差值（参考） | 100–104 KB（CRT/加载器页，见 §2.4） |

min 体积（链接 examples/minimal.c，/O1 + /OPT:REF /OPT:ICF）：
`.text` = 0x4FCC = **20,428 B**（Phase 1：20,560 B，差 0.6%）。

## 5. 偏差分析

### 5.1 单条延迟（SPSC 无锁）未达 100 ns

与 Phase 1（Linux/WSL2，P50 196–206 ns）同构偏慢 ~50%。分解：

| 组件 | Windows 实测 |
|------|--------------|
| 采样开销（2×QPC） | ~40 ns |
| 时间戳捕获（GetSystemTimePreciseAsFileTime） | ~40–50 ns（Linux clock_gettime ~20 ns 的 2 倍；GSPAsFT 走系统调用路径） |
| %msg 渲染（vsnprintf 63B，MSVC CRT） | ~25 ns |
| 环入队 + TLS + 过滤 + 原子计数 | ~35 ns |
| 其余（调用开销、结构填充） | 余量 |

**主要差异来源是实时钟捕获**：`GetSystemTimePreciseAsFileTime` 的
精度-开销权衡比 Linux vDSO 更重；这是规范管线（§4.9 生产者恒捕获
REALTIME，决策 8）在 Windows 上的固有成本。

**改进建议**：
1. 实时钟按秒缓存 + QPC 差值插值（预计算下一秒边界，热路径仅在
   跨秒时调用 GSPAsFT）——预期 P50 降至 ~120 ns；
2. 摊销口径已显示纯管线 ~113–155 ns，若再以 rdtsc 替代 QPC 采样
   计量，报告口径可再降 ~40 ns；
3. MSVC CRT 的 vsnprintf 慢于 glibc，可引入自研 64B 快速格式化。

### 5.2 MPSC 无锁 P99 ≈ 1.6 μs（目标 < 1 μs）

P50 = 500 ns 达标区间内；P99 超 60%（Phase 1 为 1.0–1.3 μs，超
0–32%）。8 生产者 CAS 竞争在 Windows 上的缓存行仲裁与内核唤醒
路径成本更高；P999 达 2.9–13.4 μs 显示偶发调度片尾。本机为
24C/28T 移动 CPU，未做核亲和绑定。

**改进建议**：生产者分组 staging（批间合并 CAS）；`SetThreadAffinityMask`
固定生产者到 E-core/P-core 组；`InterlockedCompareExchange128`
写合并。

### 5.3 MPSC 有锁 P99 ≈ 23 μs（目标 < 10 μs）

P50 = 1.9–2.0 μs；P99 尖峰机制与 Phase 1（34 μs）相同：消费者在
批量边界持环 CRITICAL_SECTION 拷贝记录 + 每 10ms flush 的输出写
与 8 生产者锁竞争叠加；Windows 的 CRITICAL_SECTION 争用自旋后
进入内核等待，唤醒延迟长于 futex。

**改进建议**（与 Phase 1 相同 + Windows 特有）：
1. 消费者批量拷出改双缓冲 + 指针交换；
2. flush 与取队解耦；
3. CRITICAL_SECTION 自旋计数调优（`InitializeCriticalSectionAndSpinCount`
   预设较高自旋值，减少内核往返）。

### 5.4 未达标项说明

三项延迟指标未达标的原因均为**平台固有成本 + 规范要求的管线步骤**
（时间戳捕获、errno 保持、原子统计），与 Phase 1 的结论一致且数值
量级相同；吞吐与内存/体积指标全部达标。24h 压测见 §7。

## 6. 与 Phase 1（Linux / WSL2）同口径对比

| 指标 | Phase 1（WSL2/GCC） | Phase 2（Win 原生/MSVC） | 变化 |
|------|--------------------|--------------------------|------|
| SPSC 逐条 P50 | 196–206 ns | 200–300 ns | +0–50% |
| SPSC 摊销 | 168–175 ns | 113–155 ns | **-15–35%**（原生调度收益） |
| 8P 无锁 P99 | 1.0–1.3 μs | 1.5–1.6 μs | +25–60% |
| 8P 有锁 P99 | ≈34 μs | 21.6–30.4 μs | -10–35% |
| 吞吐峰值 | 1.88M logs/sec | **5.16M logs/sec** | **2.7×**（原生 vs 虚拟化 + 批量路径优化） |
| 基线内存（堆/私有口径） | 6 KB（mallinfo2） | 8 KB（Private commit） | +2 KB |
| min .text | 20,560 B | 20,428 B | -0.6% |

结论：摊销延迟与有锁 P99 在原生 Windows 上改善（WSL2 虚拟化开销
消除）；逐条 P50 与无锁 P99 受 Windows 时钟 API 与 CAS 仲裁成本
影响而略高；吞吐大幅领先（虚拟化 I/O 与调度开销消除）。

## 7. 24 小时压测

**发布前门禁项，本次不执行**（任务书 §8/§13.3）。执行方法：
`tests/bench/bench_log`（3 构建变体）8 生产者连续 24h，配合
`hpulogc_get_stats()` 周期采样（accepted/written/dropped 增量连续性）
+ ASan 常驻（`HPULOGC_SANITIZER=address` 构建）验证无泄漏。

## 8. CI 基线建议

吞吐基线 5.16M logs/sec（lockfree MPSC，最优口径取 4.68M 中位更稳）：
CI 中吞吐下降 > 10%（< 4.2M）告警（§8 要求）。
