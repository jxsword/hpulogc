# hpulogc 多 Sink 体系——需求与设计（Phase 5）

> **文档定位**：本文是多 sink 体系的需求与设计规范（含 §4.7 重写、新增
> §4.10/§4.11），经确认后全部内容并入 `docs/rd_v0.6.md` 成为唯一规范性
> 文档；未触及条款沿用 rd_v0.2（并随 v0.6 合并）。设计依据以对话确认内容
> 为准（草稿中 `hplogc_` 前缀按决策 D-S6 统一改写为 `hpulogc_`）。
>
> 用户决策记录：`docs/decision_log.md`（D-M1..M4、D-S1..S7，含全部候选
> 选项与弃用理由）；本文 §8 为决策落地表。

---

## 1. 术语与总体架构

| 术语 | 定义 |
|------|------|
| **sink 类型** | 一种输出实现，由操作表 `hpulogc_sink_ops_t` 描述（vtable + 能力位 + 私有数据大小） |
| **sink 实例** | 类型的一次具名实例化：名字 + 键值配置 + 私有状态（运行时句柄 `hpulogc_sink_t`） |
| **共享环** | 全库唯一的日志记录环形缓冲（§4.3），MPMC 扩展见 §4.2；全局溢出策略仅作用于它 |
| **第二级队列** | `async=on` 的 sink 实例各自的专属有界字节环队列 + 专属 worker 线程（§4.10.6） |
| **事件** | `hpulogc_event_t`：投递给 sink 的一次日志事件（记录元数据 + 已渲染行 + 结构化字段） |

两级投递架构：

```
生产者线程们 ──渲染/字段校验──▶ [共享环 ×1（全局溢出策略）]
                                        │
                        异步构建：消费者线程(1..N)；同步构建：调用方线程
                                        │
                     路由（规则首个匹配 → 输出名列表，沿用 v0.2 §10.3）
                                        │
        ┌───────────────────────────────┴──────────────────────────────┐
        │ async=off 的 sink：当前线程内联调用 emit（conf_lock 下串行）  │
        │ async=on  的 sink：打包入第二级队列（满则丢弃计数）           │
        │                     └─▶ 专属 worker：批量出队 → emit_batch    │
        └───────────────────────────────────────────────────────────────┘
```

**不变式**：共享环与其全局溢出策略的行为与 v0.2/rd_v0.3 完全一致；多 sink
体系只改变「共享环之后」的投递结构。全部 sink 均为 `async=off` 时，运行时
行为与现行版本等价（垫片兼容的验收基准）。

---

## 2. §4.7 输出目标：多 sink 体系（规范性，重写）

> 现行版本实现的「output = 固定枚举 + 扁平结构体」自 v0.6 起**废除**。
> 输出目标统一由 **sink** 表达：一个 sink = 一种类型（`hpulogc_sink_ops_t`，
> §4.10）+ 一个实例（名字 + 私有配置 + 私有状态）。§4.10 定义 sink 契约，
> 本节定义随库提供的**内置 sink 清单与各自的配置键**。

### 2.1 内置 sink 清单（§4.7.1）

| sink type | 配置别名 | 说明 | 能力位 | 依赖 | 裁剪选项 |
|-----------|----------|------|--------|------|----------|
| `console` | — | stdout/stderr 输出；ANSI 彩色（仅终端设备，`HPULOGC_ENABLE_COLOR`） | `SYNC \| ASYNC` | 无 | 不可裁剪（默认配置的兜底 sink） |
| `rollingfile` | `file` | 文件写入，含轮转（size/time/both）、备份数、命名模板、`.latest` 软链、fsync | `SYNC \| ASYNC \| LINE_ATOMIC \| FSYNC` | 无 | 轮转受 `HPULOGC_ENABLE_ROTATE` 裁剪；sink 本体不可裁剪 |
| `syslog` | — | POSIX `openlog`/`syslog` 直通 | `SYNC` | libc syslog | `HPULOGC_SINKS` 未含该类型即为排除；**Windows 上该类型不可注册** |
| `null` | — | 丢弃一切（测试/基线；也是「审计/指标下线」的路由落点，§7.3） | `SYNC \| ASYNC` | 无 | **默认注册**；`min` 预设排除（§5） |
| `tcp` | — | TCP 行帧输出（每记录追加 `\n`）；断线指数退避重连（rd_v0.6 §4.10.8，v0.6.2 起内置） | `SYNC \| ASYNC \| LINE_ATOMIC` | POSIX socket | `HPULOGC_SINKS` 未含该类型即为排除；**Windows 上该类型不可注册（v1 POSIX-only）** |
| `udp` | — | UDP 数据报输出（每记录恰一个数据报；MTU 截断，rd_v0.6 §4.10.8） | `SYNC \| ASYNC \| LINE_ATOMIC` | POSIX socket | 同 `tcp` |
| `unix` | — | Unix domain socket 客户端输出（connect 型，库不创建端点；dgram 每记录一个数据报 / stream 同 TCP 行帧，rd_v0.6 §4.10.9，v0.6.7 起内置） | `SYNC \| ASYNC \| LINE_ATOMIC` | POSIX UDS | `HPULOGC_SINKS` 未含该类型即为排除；**Windows 上该类型不可注册（POSIX-only）** |
| `fifo` | — | 命名管道（FIFO）写端输出（恒 `O_NONBLOCK`；无读取者 ENXIO 延迟重试不 fail-fast；不做 popen，rd_v0.6 §4.10.9） | `SYNC \| ASYNC \| LINE_ATOMIC` | POSIX FIFO | 同 `unix` |

- `file` 是 `rollingfile` 的**配置别名**：仅把 `rotate` 的**默认值**设为
  `none`，其余键与 `rollingfile` 完全一致；显式书写 `rotate=size|time|both`
  时以显式值为准（别名与显式 `rotate` 可共存，不视为冲突）。保留该别名以
  保证 v0.1/v0.2 配置文件可直接迁移。
- `HPULOGC_MAX_SINKS`（默认 16，沿用并取代 `HPULOGC_MAX_OUTPUTS` 的语义，
  旧宏名保留为别名）限制 sink **实例**数量；`HPULOGC_MAX_SINK_TYPES`（默认
  32）限制**类型**数量（内置 + 自定义）。
- `[outputs]` 中定义的所有 rollingfile 类 sink 在 init 时即打开（沿用
  fail-fast 语义，§4.10.2）；打开失败使 init 失败（`HPULOGC_ERR_IO`）。
- **Socket 输出**：v0.1 的 socket 骨架预留**删除**（现行代码已 fail-fast，
  措辞改为「未注册类型」统一处理）。需要网络输出时以**自定义 sink** 形态
  实现（§4.10.5），在 `[outputs]` 中 `type = <自定义类型>`；未注册的类型
  一律 fail-fast（§10.4）。

### 2.2 内置 sink 的配置键（§4.7.2）

通用键（由核心消费，**不**传给 sink 的 `configure`）见 §4.7.3；下表为内置
sink 的**私有键**（逐项交给 `configure`）：

| sink | 键 | 含义 | 默认值 | 平台差异 |
|------|----|------|--------|----------|
| console | `stream` | `stdout` / `stderr` | `stdout` | — |
| console | `color` | 是否启用 ANSI 彩色 | 由 `HPULOGC_ENABLE_COLOR` 决定 | Windows 需 VT 处理（§16.2，现状保持） |
| rollingfile | `path` | 日志文件路径 | 必填 | — |
| rollingfile | `rotate` | `none` / `size` / `time` / `both` | `none` | — |
| rollingfile | `max size` | 按大小轮转阈值（支持尺寸后缀） | 必填（rotate 含 size 时） | — |
| rollingfile | `time unit` | `hour` / `day` / `week` / `month` | 必填（rotate 含 time 时） | — |
| rollingfile | `max files` | 备份数量，0 = 不限制 | 0 | — |
| rollingfile | `fsync` | 每条日志 fsync（与全局 `crash_safety` 取更严格者） | false | — |
| rollingfile | `symlink latest` | 创建 `.latest` 软链 | false | Windows 忽略并输出警告 |
| rollingfile | `rotate naming` | 归档命名模板 | `{base}.{timestamp}.{index}.log` | — |
| rollingfile | `file perms` | 文件权限（八进制） | 0644 | Windows 忽略并输出警告 |
| rollingfile | `dir perms` | 目录权限（八进制） | 0755 | Windows 忽略并输出警告 |
| syslog | `facility` | syslog facility（`user`/`daemon`/`local0`..`local7` 等） | `user` | Windows 上该类型不可注册 |

- 上述键即 v0.2 `hpulogc_output_t` 的字段集合；`rotate` / `time unit` 的
  取值集合沿用 `hpulogc_rotate_t` / `hpulogc_time_unit_t` 两个枚举（二者
  保留在公共头，作为 rollingfile 的**配置值词表**，不再作为公共结构体字段
  类型，见 §6 兼容）。
- 未识别的键交由 `configure` 返回非 0，由核心按 `strict init` 处理
  （§10.1 未知键流程）：strict = 该实例定义失败（进而 init 失败，
  `HPULOGC_ERR_CONFIG`）；lenient = stderr 告警 + 忽略。

### 2.3 sink 实例通用键（新增，规范性）

每个 sink 实例（INI 与代码内声明同构）额外支持以下**通用键**，由核心在
`configure` 之前消费：

| 键 | 含义 | 默认值 | 约束 |
|----|------|--------|------|
| `enabled` | 实例是否启用 | `true` | `false` 的实例**不打开、不参与路由**；热重载可翻转（翻转即时生效，不需要重开资源） |
| `async` | 是否异步投递 | `off` | `on` 时该实例获得第二级队列 + 专属 worker（§4.10.6）；任意构建可用（含同步构建） |
| `queue size` | 第二级队列字节容量 | 256KB（支持尺寸后缀） | 范围 64KB–16MB；仅 `async=on` 时有效，`async=off` 时给出该键为无害（告警忽略，lenient） |
| `filter keys` | 结构化字段白名单（逗号分隔字段名）：投递到该实例的事件只保留列出的字段 | 空（不过滤） | 行内含逗号须双引号包裹；只作用于结构化字段区，格式串整行不受影响；被滤除字段计入该实例 `fields_dropped`（§3.3） |

- 通用键的值非法（如 `enabled=maybe`、`queue size` 越界、`filter keys`
  超 `HPULOGC_MAX_SINK_FILTER_KEYS`（默认 8，公共可覆盖宏）项）视同
  未知键：按 `strict init` 成败。
- `async=on` + 恒 discard 溢出（§4.10.6）：**per-sink 异步投递不保证不丢**；
  需要不丢的场景使用 `async=off`（同步直写，受 reopen-once/lost 记账保护）。

### 2.4 类型注册与裁剪（§4.7.3，新增）

- 构建选项 **`HPULOGC_SINKS`**（CMake string，逗号分隔白名单，默认
  `console,rollingfile,syslog,null`；Windows 自动移除 `syslog`）决定哪些
  内置类型被编译进库；未列入的类型：注册表不含之，配置引用时报
  「未注册类型」fail-fast。
- `min` 预设强制 `HPULOGC_SINKS=console,rollingfile`（§5）；其余预设不
  改动该选项。
- 自定义类型一经 `hpulogc_sink_register` 注册（§4.10.5）即参与路由，
  不受 `HPULOGC_SINKS` 约束（该选项仅裁剪内置类型）。

---

## 3. §4.10 Sink 契约（新增，规范性）

### 3.1 操作表与 ABI（§4.10.1）

```c
struct hpulogc_sink;

/**
 * @brief sink 句柄（不透明指针）。
 *
 * 内部结构对外不可见（§15：运行时句柄一律使用 opaque pointer），私有数据
 * 通过 hpulogc_sink_priv() 访问。
 */
typedef struct hpulogc_sink hpulogc_sink_t;

/**
 * @brief sink 操作表（vtable）。
 *
 * **ABI 契约（规范性）**：
 * - 字段顺序在 HPULOGC_SINK_ABI_VERSION 主版本内冻结；新增回调只能追加到
 *   reserved 之前（即消费 reserved 槽位）。
 * - 注册时必须填写 HPULOGC_SINK_ABI_VERSION（当前 = 1）到 abi_version。
 * - type 必须在当前进程注册表内唯一且长度不超过 HPULOGC_MAX_NAME_LEN。
 */
typedef struct hpulogc_sink_ops {
    const char* type;        /*!< 配置里的 type 名，例如 "rollingfile" */
    uint32_t    abi_version; /*!< 填 HPULOGC_SINK_ABI_VERSION */
    uint32_t    caps;        /*!< HPULOGC_CAP_* 组合 */
    size_t      priv_size;   /*!< 私有数据大小；核心在 create 时分配并清零，0 = 无私有数据 */

    int  (*configure )(struct hpulogc_sink* sink, const char* key, const char* val);
    int  (*init      )(struct hpulogc_sink* sink);
    int  (*start     )(struct hpulogc_sink* sink);
    void (*emit      )(struct hpulogc_sink* sink, const hpulogc_event_t* ev);
    int  (*emit_batch)(struct hpulogc_sink* sink, const hpulogc_event_t* const* evs, size_t n);
    int  (*flush     )(struct hpulogc_sink* sink);
    int  (*sync      )(struct hpulogc_sink* sink);   /*!< flush + fsync（FSYNC 能力类型实现；NULL = 无 fsync 语义） */
    void (*destroy   )(struct hpulogc_sink* sink);

    void (*reserved[4])(void); /*!< ABI 扩展槽；必须全部为 NULL */
} hpulogc_sink_ops_t;
```

能力位（`HPULOGC_CAP_*`，公共头常量）：

| 位 | 值 | 含义 |
|----|----|------|
| `HPULOGC_CAP_SYNC` | 1 | 可在同步管道（无共享环）中使用 |
| `HPULOGC_CAP_ASYNC` | 2 | 可在异步管道（共享环 + 消费者）中使用 |
| `HPULOGC_CAP_LINE_ATOMIC` | 4 | 保证单条记录在输出流中连续完整、不被撕裂或交错 |
| `HPULOGC_CAP_FSYNC` | 8 | 支持 fsync 语义（`fsync` 键与全局 `crash_safety` 生效） |

- 能力位是**声明性**的：核心据此校验组合合法性（如仅声明 `SYNC` 的类型在
  异步管道中引用 → init 失败 `HPULOGC_ERR_CONFIG`），不改变投递机制。
- 回调可空性：`configure/init/start/flush/destroy` 均可 NULL（语义见
  §4.10.2）；`emit` 与 `emit_batch` **必须至少实现其一**，注册时校验。

### 3.2 生命周期（§4.10.2，规范性）

```
create -> configure* -> init -> start -> emit / emit_batch* -> flush -> destroy
```

| 阶段 | 调用方/时机 | 语义 |
|------|-------------|------|
| `create` | 核心；实例定义解析后 | 分配实例本体与清零的私有数据（`priv_size`）；失败 → `HPULOGC_ERR_NO_MEM` |
| `configure` | 核心；解析期逐键 | 灌入私有配置；**通用键（enabled/async/queue size/filter keys）已被核心先行消费**；未识别键返回非 0 → strict 成败 / lenient 告警忽略；解析期校验错误使 init 失败 `HPULOGC_ERR_CONFIG` |
| `init` | 核心；**全部 configure 之后**、首次 emit 前，一次 | 默认填充与资源预检。**警告**：`init` 内只能写 `if (p->field == 0) p->field = default;` 形式的默认值填充，不得无条件赋值（会清掉 configure 灌入的值）；失败 → 该实例启动失败，init 整体失败 `HPULOGC_ERR_CONFIG` |
| `start` | 核心；init 后 | 打开真实资源（文件/连接）；可为 NULL（init 已含打开）；失败 → `HPULOGC_ERR_IO`（fail-fast，沿用 rollingfile 打开语义） |
| `emit` / `emit_batch` | 投递路径（§4.10.4/§4.10.6） | 写出事件；`emit` 不得返回失败，失败通过 per-sink 统计暴露（§4.10.3） |
| `flush` | flush/sync/周期 idle | 冲刷缓冲（仅写出/提交，**不含 fsync**）；可为 NULL（无缓冲语义）；返回非 0 = 仍有残留或失败 |
| `sync` | `hpulogc_sync()` 调用 | flush + fsync；仅声明 `HPULOGC_CAP_FSYNC` 的类型需要实现（NULL = 无 fsync 语义，核心仅 flush）；现行 `hpu_output_sync` 语义平移 |
| `destroy` | 关闭/热重载替换 | 释放私有资源；核心在调用后释放实例本体；**核心保证幂等且 destroy 返回后不得再触碰 sink** |

- **核心额外保证**：关闭前必然先调用 `flush`（等价现行 `hpu_output_close`
  语义）；全局 `crash_safety` 与 per-sink `fsync` 键的严格度组合在 init
  阶段换算为 sink 私有状态（现行 `fsync_sev` 机制平移），`sync` 回调按
  该状态决定实际落盘行为。
- **syslog 特例**：进程全局状态（openlog ident/tag），单实例限制：注册表
  层面保证同类型 syslog 实例数 ≤ 1，第二个实例定义 → init 失败
  `HPULOGC_ERR_CONFIG`（错误信息明示）。

### 3.3 记账规则（§4.10.3，规范性）

每个 sink 实例维护 `hpulogc_sink_stats_t`（可从任意线程原子读取）：

```c
typedef struct {
    unsigned long long written;        /*!< 该 sink 已成功投递（含入缓冲）的记录数 */
    unsigned long long dropped;        /*!< 投递到该 sink 前被丢弃的记录数（队列满 / 异步背压） */
    unsigned long long failed;         /*!< 写失败（含重试后仍失败）的记录数 */
    unsigned long long fields_dropped; /*!< 因 per-sink 过滤（§2.3 filter keys）被滤除的结构化字段数 */
    unsigned long long bytes_written;  /*!< 累计写出字节数（按事件行字节数累计） */
} hpulogc_sink_stats_t;
```

- `written`：核心在每次成功 `emit` 后 +1（`emit_batch` 成功子集逐条 +1）。
- `dropped`：仅第二级队列丢弃时由核心累加（§4.10.6）；同步直写路径恒 0。
- `failed`：sink 内部写失败时通过核心提供的内部记账入口累加（rollingfile
  的 lost 计数平移为 failed）；核心在 sink 关闭/替换时把未决 failed 并入
  全局 `retired_lost`（决策 16 迁移通道），保证全局统计恒等式闭合：
  `accepted = written + dropped + overwritten + throttled + 在途`。
- `fields_dropped`：记录**因该实例 `filter keys` 白名单（§2.3）被滤除**
  的结构化字段数（每实例独立，逐字段 +1）。与全局 `fields_dropped`
  （§4.11.2 生产端三成因，append-only ABI）**正交、不重复计数**；
  未配置 `filter keys` 的实例恒 0。公共 API
  `hpulogc_get_sink_stats(name, &stats)` 读取
  实例统计；`name` 未定义或未初始化 → `HPULOGC_ERR_INVALID_ARG` /
  `HPULOGC_ERR_STATE`。

### 3.4 线程模型与重入约束（规范性）

- **串行化承诺**：同一 sink 实例的 `emit/emit_batch/flush` 回调**至多被
  一个线程同时执行**。实现依据：同步路径与消费者路径全程持有 conf_lock
  （现状），第二级队列的 worker 每实例唯一。自定义 sink 因此**无需自行
  加锁**。
- **重入禁止**：回调中禁止调用除 `hpulogc_sink_*` 之外的库 API（避免
  重入死锁）；除首次私有初始化外，回调内不得动态分配内存（§9 稳态零
  malloc）。
- **事件有效性**：`emit/emit_batch` 收到的 `hpulogc_event_t` 及其全部
  指针**仅在本次回调内有效**；跨出回调保留必须深拷贝。
- **register 线程模型**：`hpulogc_sink_register` 仅可在 init 前单线程调用
  （与 init/shutdown 同一「不被承诺并发」域）；init 后调用返回
  `HPULOGC_ERR_STATE`。

### 3.5 自定义 sink 的注册与代码内配置（规范性）

- **注册 API**：`int hpulogc_sink_register(const hpulogc_sink_ops_t* ops);`
  —— 仅可在 init 前单线程调用（init 后调用返回 `HPULOGC_ERR_STATE`）。
  校验失败返回 `HPULOGC_ERR_INVALID_ARG`：`ops` 为 NULL、`type` 为 NULL/空/
  超 `HPULOGC_MAX_NAME_LEN`、`abi_version` 不等于
  `HPULOGC_SINK_ABI_VERSION`、`emit` 与 `emit_batch` 均为 NULL、reserved
  槽位非全 NULL。类型名重复或类型表超 `HPULOGC_MAX_SINK_TYPES`（32，含
  内置类型）返回 `HPULOGC_ERR_CONFIG`。
- **注册时机与形态**：v1 仅支持**静态注册**（进程内链接的自定义实现，init
  前显式调用注册）；不提供 dlopen/LoadLibrary 动态发现机制（与全库零
  依赖定位一致，附录 A.1-10 的 record 回调形态由本契约覆盖）。
- **代码内配置**：`hpulogc_config_t.sinks[]`（`hpulogc_sink_decl_t`，§5）
  与 INI `[outputs]` 行内定义同构；键值对逐项经 `configure` 灌入，通用键
  （enabled/async/queue size/filter keys）由核心先行消费。`sinks` 与 `outputs`（垫片）
  可混用，实例名合计受 `HPULOGC_MAX_SINKS` 约束；与 `[outputs]`（INI）或
  彼此重名 → init 失败 `HPULOGC_ERR_CONFIG`。
- **实现约束**（重申 §4.10.4）：回调内禁止调用除 `hpulogc_sink_*` 外的库
  API；稳态零 malloc；事件指针仅回调内有效；跨回调保留需深拷贝。

### 3.6 per-sink 异步投递（§4.10.6，规范性）

- **第二级队列**：每个 `async=on` 实例一个专属有界**字节**环队列（容量
  `queue size`），互斥量 + 条件变量实现，事件以紧凑打包格式写入（头 +
  字符串 + 字段区，与 §4.11.4 同构）；**满则丢弃当前事件并使该实例
  `dropped +1`**（恒 discard，不阻塞投递方；D-S5）。
- **worker**：每实例一个专属线程；循环批量出队（单批上限
  `batch_size` 条，复用全局批量口径），组事件指针数组调用
  `emit_batch`（未实现 `emit_batch` 的类型由核心循环 `emit`，并按
  `emit_batch` 返回值规则记账：返回成功条数 n → n 计 written、余量计
  failed；负值 → 全批计 failed），随后按 `flush_interval_ms` 周期
  `flush`。
- **投递方**：异步构建 = 消费者线程；同步构建 = 调用方线程。打包入队为
  非阻塞操作。
- **flush/sync 聚合**：`hpulogc_flush()` 等待「共享环空 && 全部消费者
  ack（rd_v0.3）&& 全部 async 队列空 && 全部 worker 完成 flush」；
  `hpulogc_sync()` 在此之上要求 `HPULOGC_CAP_FSYNC` sink 落盘。等待上限
  沿用 `shutdown_timeout_ms`（0 = 无限）；超时按现有 best-effort 语义
  返回 `HPULOGC_ERR_IO`。
- **shutdown 聚合**：置停止位（停投递）→ 唤醒全部 worker → 各队列排空
  → worker flush → join 全部 worker（有界，超时按现行为继续销毁）→
  sink destroy。顺序上 async worker 先于消费者线程汇合点之前停止接收，
  保证已入队事件被处理。
- **热重载**：实例的 type/enabled/async/queue size/filter keys 或任何私有键变化 →
  该实例**销毁重建**（worker 停止→排空→flush→destroy→新建）；rollingfile
  满足「path + 全参数相等」且 async 属性不变 → 复用现 fd（现行
  reuse_old_idx 语义平移）；console/syslog/null 一律重建（廉价且无状态）。
  `enabled` 翻转（其余不变）不重建：仅改变路由可见性。

### 3.7 核心分发层的实现映射（非规范性）

- 现行 `hpu_output_base_t`（type tag + name + fsync_sev）扩展为
  `{ops 指针, name, fsync_sev, 实例统计, enabled/async/queue 元数据}`；
  `output.c` 分发器的 `switch(type)` 全部替换为 ops 调用，文件/控制台后端
  行为**零变化**（缓冲阈值、reopen-once、fsync 严重度、轮转钩子、lost→
  failed 记账逐一平移）。
- `hpu_output_write_direct()`（探索确认无调用者）随本次重构删除。
- `hpu_io_fail_hook` 注入点保留并平移进 rollingfile 的实现（测试依赖）。

---

## 4. §4.11 事件与结构化字段（新增，规范性）

### 4.1 事件结构（§4.11.1）

```c
typedef enum {
    HPULOGC_FIELD_I64  = 0, /*!< int64_t */
    HPULOGC_FIELD_U64  = 1, /*!< uint64_t */
    HPULOGC_FIELD_F64  = 2, /*!< double */
    HPULOGC_FIELD_BOOL = 3, /*!< int（0/1） */
    HPULOGC_FIELD_STR  = 4  /*!< 字节串（非 NUL 结尾，带长度） */
} hpulogc_field_type_t;

typedef struct hpulogc_field_value {
    hpulogc_field_type_t type;
    union {
        int64_t  i64;
        uint64_t u64;
        double   f64;
        int      b;
        struct { const char* s; size_t len; } str; /*!< 调用方在 log 调用返回前有效 */
    } v;
} hpulogc_field_value_t;

typedef struct hpulogc_field {
    const char*           key;    /*!< 字段名（长度上限见 §4.11.2） */
    hpulogc_field_value_t value;
} hpulogc_field_t;

typedef struct hpulogc_event {
    /* 记录元数据（与 hpu_log_record_t 同义，跨 ABI 不共享类型） */
    int         level;
    const char* category;  size_t category_len;
    const char* file;      size_t file_len;
    const char* func;      size_t func_len;
    int         src_line;  /*!< 源代码行号 */
    const char* msg;       size_t msg_len;   /*!< 已渲染消息体 */
    uint64_t    tid;
    int64_t     realtime_ns;
    int64_t     mono_us;
    /* 本规则已渲染的完整行（文本 sink 的主要消费形态） */
    const char* line;      size_t line_len;
    /* 结构化字段（视图；仅回调内有效） */
    const hpulogc_field_t* fields;
    size_t      field_count;
    const uint8_t* fields_wire;  /*!< 线格式字段区视图（§4.11.4） */
    size_t      fields_len;      /*!< 线格式字节计数 */
} hpulogc_event_t;
```

- `line/line_len`：按该记录命中规则（或默认回退）的格式渲染的整行（与现行
  `hpu_output_write_line` 收到的字节一致）；多 sink 同规则共享同一行视图。
- 同步模式下事件在调用方线程构造；异步模式下消费者线程从共享环视图重建。

### 4.2 字段预算与 fields_dropped（§4.11.2，规范性）

公共可覆盖宏（默认值）：

| 宏 | 默认 | 语义 |
|----|------|------|
| `HPULOGC_MAX_FIELDS` | 16 | 单条事件字段数上限 |
| `HPULOGC_MAX_FIELD_STR_LEN` | 256 | STR 值字节上限（超长**截断**） |
| `HPULOGC_MAX_FIELD_KEY_LEN` | 64 | 字段名字节上限（超限**整字段丢弃**） |
| 字段区预算 | `max_log_length` | 单条事件序列化后的字段区总字节 ≤ `max_log_length`（超限从尾部逐字段丢弃） |

`fields_dropped` 计数三成因（每条记录独立累加，进入全局统计，§4.10.3）：

1. 字段数超过 `HPULOGC_MAX_FIELDS`：多出字段整体丢弃，逐个 +1；
2. STR 值超 `HPULOGC_MAX_FIELD_STR_LEN`：截断保留前缀，该字段 +1；
3. 字段区序列化总量超 `max_log_length` 预算：从尾部逐字段丢弃，逐个 +1；
   字段名超 `HPULOGC_MAX_FIELD_KEY_LEN` 同理整字段丢弃 +1。

预算检查发生在**生产端、入环之前**（与 §4.9④ 同层），保证环内记录恒满足
预算（「任何单条记录必能入环」不变式的 `max_record_size` 公式相应扩展，
见 §4.11.4）。

### 4.3 生产端 API（§4.11.3，规范性）

```c
HPULOGC_API void hpulogc_log_ex(hpulogc_level_t level, const char* category,
                                const char* file, int line, const char* func,
                                const hpulogc_field_t* fields, size_t field_count,
                                const char* fmt, ...);
HPULOGC_API void hpulogc_vlog_ex(hpulogc_level_t level, const char* category,
                                 const char* file, int line, const char* func,
                                 const hpulogc_field_t* fields, size_t field_count,
                                 const char* fmt, va_list ap);
```

- 便捷宏族（经既有 `HPULOGC_VA_ARGS` 可移植垫片）：
  `HPULOGC_TRACE_EX(cat, flds, n, ...)` … `HPULOGC_FATAL_EX(...)`；
- 既有 `HPULOGC_INFO` 等宏族**不变**（等价 fields=NULL, 0）；
- 限流/采样/级别过滤/errno 保护语义与既有路径完全一致；
- **signal-safe 通道不支持字段**（`hpulogc_log_signal_safe` 保持原样；
  async-signal-safe 域禁止遍历字段）。

### 4.4 环形缓冲格式扩展（§4.11.4，规范性）

- 记录头 `hpu_ring_meta_t` 的最后一个 16 位保留字段改用为 **field_count**
  （u16）；**头部大小不变**（编译期断言保证 locked/lockfree 布局一致）。
- 字段区追加在字符串区（category/file/func/msg）之后，逐字段：
  `{u16 key_len; u8 type; u8 pad; u16 val_len; u8 val[val_len];}`，
  I64/U64/F64 恒 `val_len=8`（宿主字节序 memcpy，同进程语义），BOOL
  `val_len=1`，STR `val_len=字节串长度`；字段区整体 8 字节对齐收尾。
- `max_record_size` 公式扩展：`sizeof(meta) + max_log_length + 256 + 512
  + 256 + 8` 之上追加字段区上界
  `align8(HPULOGC_MAX_FIELDS × (6 + HPULOGC_MAX_FIELD_KEY_LEN +
  HPULOGC_MAX_FIELD_STR_LEN))`；「buffer size 自动提升保证任何单条记录
  可入环」不变式随之成立（conf_model 现有公式处同步）。
- locked / lockfree 双实现的记录写入/读取路径同步扩展（pad 不变式不变）；
  MPMC 认领协议（rd_v0.3 §5）不受影响。

### 4.5 渲染（§4.11.5，规范性）

- **`json` 格式**：记录含字段时，行内自动追加成员 `"fields":{"k":v,...}`
  ——键 JSON 转义；I64/U64 原生数字、F64 数值、BOOL 原生 true/false、
  STR JSON 转义字符串；**无字段时不追加该成员**（无字段记录的输出与
  现行版本逐字节一致，兼容基线）。
- **文本占位符 `%v`**（字符未与现行动作表冲突，已核实）：按序渲染
  `k1=v1 k2=v2 ...`（空格分隔）；I64/U64 十进制、F64 `%.17g`、BOOL
  `true/false`、STR 原样；键与值经注入转义规则（同 `%msg`）；无字段时
  渲染为空串。既有格式模板不自动追加 `%v`（内置五格式中仅 `json` 消费
  字段），需要字段的自定义模板显式书写 `%v`。

---

## 5. 配置键总表（汇总）

```
[outputs]
<name> = console,        stream=stdout|stderr, color=true|false, <通用键...>
<name> = rollingfile,    path=..., rotate=..., max size=..., time unit=...,
                         max files=..., fsync=..., symlink latest=...,
                         rotate naming=..., file perms=..., dir perms=...,
                         <通用键...>
<name> = file, ...            # rollingfile 别名（rotate 默认 none）
<name> = syslog,         facility=user, <通用键...>          # POSIX 专属
<name> = null,           <通用键...>
<name> = <自定义类型>,   <该类型 configure 认识的键>, <通用键...>

<通用键...> := enabled=true|false, async=on|off, queue size=<size>,
              filter keys=<逗号分隔字段名白名单>
```

代码内声明（新增，追加式 ABI）：

```c
typedef struct {
    const char*        name;   /*!< 实例名（路由引用） */
    const char*        type;   /*!< 类型名（须已注册） */
    const char* const* keys;   /*!< 私有键数组 */
    const char* const* vals;   /*!< 与 keys 等长的值数组 */
    size_t             count;
} hpulogc_sink_decl_t;

/* hpulogc_config_t 追加（append-only ABI 规则） */
    const hpulogc_sink_decl_t* sinks;      /*!< 通用 sink 声明 */
    size_t                     sink_count;
```

通用键（enabled/async/queue size/filter keys）在两种声明形态中均可书写；扁平结构
`hpulogc_output_t` 的垫片映射见 §6。

---

## 6. 兼容与迁移

| 旧元素 | 处置 |
|--------|------|
| `hpulogc_output_t`（扁平结构） | **保留为 deprecated 垫片**：code config 中每个元素内部映射为 sink 声明（CONSOLE→console(stream/color)、FILE→rollingfile(全部字段)），行为与 v0.2 逐字节一致；文档标注 deprecated，头注释说明迁移路径 |
| `hpulogc_output_type_t` | 保留（垫片与旧代码使用），不再扩展新值；新类型一律走 `sink_decl_t`/INI 字符串类型 |
| `HPULOGC_MAX_OUTPUTS` | 保留为 `HPULOGC_MAX_SINKS` 的别名（同值 16） |
| `hpulogc_rotate_t` / `hpulogc_time_unit_t` | 保留为 rollingfile 配置值词表 |
| `[outputs]` INI 语法 | 完全向后兼容：`console`/`file` 行为不变；新增类型词表与通用键；未注册类型 fail-fast（现 socket 拒绝语义泛化） |
| 全局统计 | 恒等式不变；`hpulogc_stats_t` 追加 `fields_dropped`（append-only） |
| 热重载 | 实例复用规则按 §4.10.6 泛化；`[outputs]` 变更的 reload 语义（fail-fast + all-or-nothing）不变 |

---

## 7. 测试与验收（Phase 5 DoD）

1. **单元**：类型注册表（注册/重复/超 32/ABI 版本/emit·batch 缺一）、
   null sink（written 计数）、console（流选择/颜色降级/`stream` 键）、
   字段预算（三成因逐项 + 环往返保真：locked/lockfree × SPSC/MPSC/MPMC
   下的字段不丢失不变形）、syslog（POSIX：init/close/单实例 fail-fast）。
2. **集成**：INI 全类型路由与 `file` 别名；未知类型 fail-fast；未知键
   strict/lenient；`enabled` 翻转 + 热重载重建/复用规则；per-sink 异步
   （丢弃计数、flush 排空、shutdown 排空、慢 worker 隔离、queue size
   生效）；`hpulogc_get_sink_stats` 恒等式；MPMC × 多 sink 并发（N 消费者
   × async/sync 混合 sink）无丢失无重复；垫片兼容（存量测试全绿）。
3. **消毒器与门禁**：TSan（第二级队列 + worker 必须干净）、ASan/UBSan、
   MSVC `/W4` 零警告、24 组合矩阵、三平台 CI。
4. **性能报告**：`docs/perf_report_sinks.md`（Linux 实测；场景 = sink
   分发开销：null 基线 / console / rollingfile / async rollingfile /
   多 sink 扇出）。

---

## 8. 决策项记录（落地表）

完整决策矩阵（含全部候选选项与弃用理由）见 `docs/decision_log.md`；
本文规范性内容的决策来源：

| 决策 | 结论 | 影响 |
|------|------|------|
| D-S1 事件模型 | 本期实现结构化字段 | §4 全量 |
| D-S2 字段值类型 | 带类型联合体 | §4.1 |
| D-S3 字段 API | 结构体数组入参 + EX 宏 | §4.3 |
| D-S4 per-sink async | 本期实现独立队列+worker | §3.6 |
| D-S5 队列溢出 | 恒 discard 不阻塞 | §2.3/§3.6 |
| D-S6 命名前缀 | 统一 `hpulogc_` | 全文 |
| D-S7 代码配置兼容 | 垫片 + 通用声明 | §5/§6 |

次级设计定案（本文正文中固化）：ABI 版本 1 + reserved[4]；能力位表；
`HPULOGC_SINKS` 白名单（syslog Windows 排除、min 预设裁剪）；syslog
单实例限制；热重载复用矩阵；`emit_batch` 批边界调用与记账规则；回调
串行化承诺；`queue size` 字节口径；`enabled` 语义；字段三预算宏；
ring 头 field_count 复用与字段区线格式；`max_record_size` 扩展公式；
`json` fields 成员与 `%v` 占位符；`hpulogc_get_sink_stats`；`null`
written 计数；静态注册（v1 无 dlopen）；契约并入唯一公共头 `hpulogc.h`。
