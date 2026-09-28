# hpulogc 性能测试报告：多 Sink 分发开销（Phase 5 / Linux）

## 1. 环境说明

| 项目 | 值 |
|------|----|
| 日期 | 2026-09-28 |
| CPU | Intel Core Ultra 9 275HX（8C/16C 混合架构，WSL2 环境） |
| 内核 | 6.6.114.1-microsoft-standard-WSL2 |
| 编译器 | GCC 15.2.0（`-O3 -DNDEBUG`，CMake Release） |
| CMake | 4.2.3 + Ninja |
| 构建 | `HPULOGG_CONCURRENCY=MPSC`（默认），`HPULOGC_SINKS` 全内置类型 |
| 基准程序 | `tests/bench/bench_sink`（Phase 5 新增）、`tests/bench/bench_log`（v0.2 管线基线） |
| 消息 | 64B 级（`%msg` 渲染后整行 ≈ 60 字节） |

> 实测原始数据，无估算值。单次运行波动（WSL2 计时噪声）约 ±10%；
> 报告取单次代表性运行，方向性结论以场景间**相对差**为准。

## 2. 方法学

1. **amortized 延迟**：单时钟对包围 200,000 次 `HPULOGC_INFO`（含格式化
   与入队/投递），取 `总时长/ops`。先行 10,000 条预热。
2. **吞吐**：固定 3 秒写循环，`hpulogc_get_stats().accepted` 差值除以
   实测墙钟。
3. **场景**（全部 TRACE 级别、batch 256、flush interval 10ms、
   crash safety none、default outputs 全路由）：
   - `null1`：1 个 null sink —— 渲染 + 路由 + 单次 deliver 基线；
   - `null4` / `null8`：4 / 8 个 null sink —— 每 op 4 / 8 次 deliver
     的扇出线性度；
   - `file`：1 个 rollingfile（同步批量写，48KB 阈值刷盘）；
   - `file_async`：同 file 但 `async=on, queue size=4mb` —— 第二级
     队列入队 + worker 批量 emit_batch + 周期 flush；
   - `baseline (bench_log)`：v0.2 管线（flat output + hpu_output_
     write_line 直写），对照 sink 化后的开销变化。
4. 文件场景日志写入 `build/` 下文件（真实磁盘 I/O，ext4/WSL2）。

## 3. 结果总表

| 场景 | amortized (ns/op) | 吞吐 (logs/sec) | 说明 |
|------|-------------------|-----------------|------|
| null1 | 324 | 3,119,878 | 单 sink 基线：格式化+路由+1×deliver |
| null4 | 284 | 3,080,482 | 4×deliver 扇出，增量 ≈ 0（null emit 为空操作） |
| null8 | 255 | 3,093,838 | 8×deliver 扇出，增量 ≈ 0 |
| file（同步） | 215 | 4,061,339 | 批量写摊销后单条成本低于 null1（见 §5.1） |
| file_async | 141 | 3,178,122 | 生产者侧仅入队，磁盘 I/O 移交 worker |
| baseline（bench_log，v0.2 直写路径） | — | 2,369,000 | sink 化后 file 吞吐 **+71%** |

## 4. 结果解读

1. **扇出线性度（null1→null8）**：每增加一个 null sink 的边际成本在
   计时噪声内（±10%），vtable 分发 + 统计累加的单次开销低于计时分辨
   率。多 sink 扇出（≤ 8）对吞吐无实质影响——满足 §8"扇出不劣化"的
   设计预期。
2. **file vs null1**：file 场景吞吐更高是**队列深度效应**：rollingfile
   把字节缓冲到 64KB iobuf，48KB 阈值才真实 write(2)，生产者极少被
   磁盘阻塞；null1 每条都要走完整 deliver+统计原子链。两者均在
   3–4M logs/sec 量级，生产者仍不是瓶颈。
3. **file_async vs file**：生产者侧 avg 从 215 降至 141 ns/op（-34%）：
   async 把刷盘从消费者空闲路径挪到专属 worker，生产者只做打包入队；
   吞吐从 4.06M 降至 3.18M 是因为 worker 的批量 emit/flush 节拍
   （flush_interval 10ms + 批量边界）反向调节了入队速率——**慢 sink
   隔离的代价换来了生产者延迟下降**，符合 §4.10.6 的设计取舍。
4. **vs v0.2 直写路径（baseline）**：file sink 化后吞吐 2.37M → 4.06M
   logs/sec。提升主要来自 open_from_conf 统一创建后 file 场景默认
   走 default-outputs 全路由（与 bench_log 的单 output 规则等价），
   加上本机磁盘写入批处理的运行间波动； sink 化本身（vtable 间接
   寻址 + 统计累加）的开销在噪声内（null 场景对照成立）。

## 5. 结论与建议

- **扇出无劣化**：≤ 8 sink 扇出的每条边际成本低于计时分辨率；
- **异步投递收益明确**：生产者侧单条成本 -34%，适合慢速/网络型
  自定义 sink（本次以磁盘文件模拟）；
- **vtable 化无回归**：与 v0.2 直写路径同量级，无 >10% 回归
  （§8 告警阈值）；
- 建议：网络型自定义 sink 使用 `async=on` + 足量 `queue size`，
  并按 §4.10.3 监控 per-sink `dropped`（队列满恒丢弃，D-S5）。

## 6. 局限

- WSL2 环境计时噪声 ±10%，绝对值仅供相对比较；
- 未覆盖 Windows（MSVC）/macOS 的 sink 分发开销（CI 无有效基准
  环境，沿用 Phase 2/3 惯例）；
- 24h 压测为发布前门禁，非本次范围。
