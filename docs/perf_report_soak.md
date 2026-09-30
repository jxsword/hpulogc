# hpulogc 24h 压测报告（rd_v0.6 §13.3 / 决策 D-R6 发布前门禁）

- 日期：2026-09-29 17:19 CST 启动 → 2026-09-30 结束（双轨各 86400 s）
- 库基线：commit `4c687cd`（PR #18 合并点，含 P-6 MPMC flush 握手修复）
- 执行环境：云上 juzdata（正式门禁，D-R6 注记②的环境变更）+ 本地 WSL2
  （D-R6 原定案口径，保留作对照）双轨；每轨 `default` 与 `tsan` 两配置
- **状态（2026-09-30）**：WSL 对照轨双配置 `VERDICT PASS`（本报告 §3/§5）；
  云上第一轮在 t≈18.7h 因**宿主机磁盘耗尽（环境故障）**终止且未产出
  判定（§4），已修复脚本缺陷（PR #38）并于同日 17:40 CST 以最新 main
  （@ 27fb597，覆盖 P-6 修复 + filter keys + IPC sinks）重启 24h 重跑，
  判定待回填（§4.3）。

## 1. 负载形态（D-R6 定案，双轨一致）

- 构建：MPMC 并发 + 无锁环（`HPULOGC_LOCKFREE=ON -DHPULOGC_CONCURRENCY=MPMC`）；
  `tsan` 变体为同形态 + ThreadSanitizer（`setarch $(uname -m) -R` 前缀，
  PR #18）。
- 生产/消费：4 生产者线程 × ~200 rec/s（`--rate-us 5000`，含抖动），
  4 消费者线程（`consumer threads = 4`），24 h 连续运行 ≈ 6,900 万条。
- sink 扇出：`rollingfile`（size 轮转 64 KB，max files=16，持续触发轮转）
  + `null`（**per-sink 异步队列 512 KB**，常态打满以行使二级队列与
  drop 记账，D-S5）+ `console`（stderr，由脚本重定向至文件并按小时
  截断保磁盘）。
- 热重载：维护线程每分钟改写配置文件（INFO↔DEBUG 翻转、队列容量切换）
  触发 watcher 热加载（WSL default 轨全程 1,439 次 RELOAD），期间生产
  不停；另有周期性 `hpulogc_flush()`。

## 2. 判据（三口径，样本周期 5 s）

1. **记账恒等式**：逐样本 `written + dropped + overwritten + throttled ≤
   accepted`，最终 flush/sync 排空后**全等闭合**；任一违反即
   `violations` 计数并判 FAIL。
2. **无停顿/无死锁**：全局 `written` 在相邻样本间严格递增；任何一次
   停顿记 `stalls` 并判 FAIL。
3. **无泄漏**：RSS 逐样本记录，取首/末四分位均值对比无增长趋势
   （tsan 变体基数含 shadow 映射，允许量级更大但同样要求无趋势）。

## 3. 结果总表

| 轨 | 配置 | accepted | written | dropped | stalls | violations | RSS 首均 KiB | RSS 末均 KiB | 判定 |
|----|------|----------|---------|---------|--------|------------|--------------|--------------|------|
| 云上①（环境故障终止） | default | —（t≈18.7h 起失效） | | | 磁盘满后 130 | **全程 0** | | | 无效（§4.1） |
| 云上①（环境故障终止） | tsan | —（t≈18.7h 起失效） | | | 磁盘满后 124 | **全程 0** | | | 无效（§4.1） |
| 云上②（重跑中） | default/tsan | — | | | | | | | 判定待回填（§4.3） |
| WSL | default | 69,848,936 | 69,848,936 | 0 | 0 | 0 | 4,634 | 4,636 | **PASS** |
| WSL | tsan | 69,561,739 | 69,561,739 | 0 | 0 | 0 | 42,011 | 42,286 | **PASS** |

WSL 双配置 `FINAL` 行均为 `accepted == written`（库侧零丢失：恒等式以
全等闭合，`dropped = 0`——512 KB 异步队列在该速率下未溢出）、零停顿、
零违规；RSS 末均相对首均 **+0.04%**（default）/ **+0.65%**（tsan，含
分配器 arena 波动），无增长趋势。全程各 17,280 个采样（5 s 周期 × 24 h）
无缺失。

## 4. 云上正式门禁（juzdata：Ubuntu 24.04，2C4G，内核 6.8）

### 4.1 第一轮（2026-09-29 17:19 启动，基线 `4c687cd`）——环境故障终止，无效

- 采样日志：`~/hpulogc-soak/build/soak_24h/{default,tsan}.ini.samples.log`
  （各 ~13,600 个采样，完整预期 17,280）。
- **t = 0 – 67,289 s（前 18.7 h）：完全干净**——零 STALL、零 violations、
  逐样本恒等式闭合；磁盘未满前库行为无任何异常。
- **t ≈ 67,300 s 起（09-30 ~12:00）磁盘逐步耗尽，t ≈ 70,664 s（~13:57）
  写入面崩溃**：宿主机 20G 盘被撑满（根因见 §4.1.1），console 捕获、
  rollingfile 轮转、driver 自身的采样日志与维护线程的配置改写全部
  ENOSPC 失败（samples 日志半行截断、config 留 0 字节），两变体相继
  停止采样（STALL 130/124 次全部落在此时段），进程空转至 24h 时长
  耗尽退出，**未产出 FINAL/VERDICT**。
- **记账恒等式全程（含磁盘满窗口）零 violations**；无崩溃、无挂死
  （driver 自行存活至时长结束）——库在 ENOSPC 压力下未损坏数据结构
  或违反恒等式，但该轮按判据无效。

#### 4.1.1 根因：soak 脚本截断循环失去变量作用域（已修复，PR #38）

`scripts/soak_24h.sh` 的按小时 console 捕获截断循环以
`nohup bash -c "$(declare -f truncate_loop); truncate_loop"` 分离启动，
分离后的 bash 只拿到函数体——`OUT`/`VARIANTS` 未定义，每小时的截断
重定向到不可写的 `/$variant/...` 路径且被 `|| true` 吞掉。console sink
（stderr → 捕获文件）以 ~800 行/s 累积 24 h，两变体各 ~2.9 GB，叠加
构建物与系统占用将 20 G 盘耗尽。修复：输出目录与变体列表改为参数
传入（引号展开冒烟验证），PR #38 合并（main @ 27fb597）。

### 4.2 云上残留物处置

删除两份 2.9 GB 的 console 捕获后磁盘可用 5.4 G（重跑期间每小时截断
生效，捕获稳态 ≤ ~300 MB）；旧采样日志保留（本节证据）。

### 4.3 第二轮（2026-09-30 17:40 CST 重跑，基线 main @ 27fb597）——进行中

- 覆盖范围比第一轮更严格：main 在 `4c687cd` 之后合入 filter keys
  （PR #35）与本机 IPC 型 sink（PR #36/#37），压测负载形态不变
  （rf/nul/con 配置不触及新增 sink）。
- 采样日志：`~/hpulogc-soak/build/soak/{default,tsan}.ini.samples.log`；
  预计 2026-10-01 ~17:40 CST 完成，判定与本节回填由后续会话执行。
- 重启后至本报告编制时（前 10 min）：零 dropped、零 STALL、恒等式闭合。

## 5. WSL 对照轨

| 项 | 值 |
|----|----|
| 宿主 | Ultra 9 275HX / WSL2（内核 6.6.114.1-microsoft-standard-WSL2） |
| 采样日志 | `build/soak_24h/{default,tsan}.ini.samples.log`（1.7 MB/轨） |
| 轮转行使 | rollingfile 64 KB × 24 h：持续高频轮转（每个轮转周期约数百条日志），max files=16 存量封顶 |
| 热重载行使 | 1,439 次配置改写全程无失败、无停顿 |

RSS 逐样本极值（非四分位均值口径，供参考）：default 3,520 → 4,636 KiB
（首轮采样含初始化爬坡，达峰后平稳）；tsan 33,604 → 43,616 KiB（TSan
shadow 映射随访问页渐进驻留，末四分位均值 42.3 MB 对首四分位 42.0 MB
无趋势）。

## 6. P-6 闭环注记

P-6（MPMC flush 握手缺陷，PR #17 修复）的修复效果在 WSL 双配置 24h
原始形态（拥塞 + 排空）下得到全程复核：零 `violations`（恒等式逐样本
闭合）、`hpulogc_flush()` 周期调用零失败、末次排空后
`accepted == written`。云上第一轮在磁盘耗尽窗口同样保持零 violations
（flush 语义未受 ENOSPC 破坏），但该轮无最终排空判定，不作为 P-6
证据引用。implementation_notes P-6 条目的"拥塞复现 4/60"基准自此以
24h 连续无复现作为更强证据。

## 7. 结论

- **WSL 对照轨：三口径判据全满足，`VERDICT PASS ×2`**（24h 完整，
  §3/§5）——D-R6 原定案口径下的发布前门禁证据成立。
- **云上正式门禁**：第一轮被宿主机磁盘耗尽（脚本截断缺陷，PR #38
  已修）在 18.7h 处终止，判定无效但前 18.7h 零停顿、全程零违规，
  且 ENOSPC 压力下恒等式始终闭合；重跑进行中（§4.3），判定待回填。
- `hpulogc_flush()`/P-6 修复在 WSL 全程与云上故障窗口均无违规复现
  （§6）。
- 本报告的收尾状态与 D-R6 注记同步；云上第二轮 `VERDICT` 落地后，
  §3/§4.3 补全即为最终版。
