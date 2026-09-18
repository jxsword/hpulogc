# hpulogc - 纯 C 语言高性能日志库：需求规格说明书（v0.2）

---

## 1. 项目概述

| 属性 | 描述 |
|------|------|
| **文档版本** | v0.2（2026-09-19，基于 v0.1 评审修订，变更清单见文末「修订记录」） |
| **项目名称** | hpulogc (High Performance Log in C) |
| **项目定位** | 纯 C 语言、零/最小依赖、跨平台、高性能日志库 |
| **目标场景** | 从嵌入式设备到高并发服务端，适用于对性能、体积、可移植性有严格要求的系统 |
| **语言标准** | 纯 C99 / C11 双标准支持，禁止使用任何 C++ 特性（无 `.cpp` 编译单元、无 `std::`、无模板/异常/RTTI/STL） |
| **构建系统** | CMake（全平台统一构建，支持 `find_package` 集成） |
| **许可证** | MIT（定案；LICENSE 文件与分发文案均以 MIT 为准） |

> 本文档为实现依据：所有行为定义均为规范性（normative），实现不得引入文档未定义的对外行为；实现细节（算法、内部数据结构）不在约束范围内。
>
> 配置文件的词法与解析参考 [zlog](https://github.com/HardySimpson/zlog) 的实现（见 §10.1）；zlog 最新代码中可能更优的设计对比见附录 A（待决策项，不在本文规范性范围内）。

---

## 2. 跨平台与编译器支持

### 2.1 目标平台矩阵

| 操作系统 | 编译器 | CPU 架构 | 位宽 |
|----------|--------|----------|------|
| Linux | GCC (≥ 4.9), Clang (≥ 3.5) | x86, ARM, MIPS, RISC-V 等 | 32 / 64 bit |
| macOS | Apple Clang (Xcode ≥ 10) | x86_64, ARM64 (Apple Silicon) | 64 bit |
| Windows | MSVC (≥ 2015), MinGW-w64 (GCC ≥ 7) | x86, x64, ARM64 | 32 / 64 bit |

> **编译器版本与语言标准的约束关系**：Linux GCC 最低版本取 4.9，因 C11 构建依赖 `<stdatomic.h>`（GCC 4.9 引入；C99 路径仅依赖 `__atomic_*`，GCC 4.7+ 即可）。Windows：C99 构建最低 MSVC 2015；C11 构建（`/std:c11`）需 MSVC 2019 16.8 及以上，低版本 MSVC 仅承诺 C99 构建（Windows 原子后端与语言标准无关，见 §2.2/§4.3）。

### 2.2 允许的底层设施

| 类别 | 具体设施 |
|------|----------|
| 线程与同步 | POSIX `pthread`、Win32 API（`CRITICAL_SECTION`、`SRWLOCK`、`Condition Variable`、线程 API） |
| 原子操作 | **Linux**：C11 `<stdatomic.h>`；C99 GCC/Clang `__atomic_*`（不可用时回退 `__sync_*`）。**macOS**：C11 `<stdatomic.h>`；C99 Apple Clang `__atomic_*`（禁止使用已废弃的 `OSAtomic*`）。**Windows**：全部语言标准一律使用 MSVC `Interlocked*`。后端选择矩阵见 §4.3 |
| 文件 I/O | 标准同步文件 I/O（`fopen`/`fwrite`/`write` 等同步写调用） |
| 语言标准 | C99 / C11 双标准支持，CMake `HPULOGC_C_STANDARD`（99 / 11，默认 99）选择；两种标准下原子操作均须可用（§4.3、§11） |

---

## 3. 架构设计原则

### 3.1 接口与实现分离

- **公共接口层**：统一头文件（`hpulogc.h`），声明所有对外 API、数据结构、枚举常量。
- **平台实现层**：按决策矩阵选择组织方式（见下）。

### 3.2 实现方式决策矩阵

| 场景分类 | 实现策略 | 理由 |
|----------|----------|------|
| 线程管理、文件 I/O、时间处理、路径/目录、文件监视（热加载） | ✅ **CMake + 按平台分目录独立 `.c` 文件**（§3.3） | 架构级差异，代码量大、逻辑复杂，独立文件便于维护 |
| 环形缓冲（有锁 / 无锁） | ✅ **CMake + 独立 `.c` 文件**（`ringbuf_locked.c` / `ringbuf_lockfree.c`，二选一编译） | 两套完整算法实现，独立演进、独立测试，避免巨型 `#ifdef` 单文件 |
| 原子操作后端 | ✅ **按后端独立头文件**（`atomic_stdatomic.h` / `atomic_gcc.h` / `atomic_msvc.h`，CMake 选择） | 平台 × 语言标准组合多（Linux/macOS × C99/C11 + Windows），单文件 `#ifdef` 膨胀难维护 |
| 结构体定义（平台相关字段） | ✅ **头文件 + `#ifdef`** | 差异修补，保持声明统一 |
| 功能开关（裁剪控制） | ✅ **`#ifdef` 编译宏** | 编译期裁剪，零运行时开销 |
| 热路径小函数 | ✅ **`inline` + `#ifdef`** | 避免函数调用开销 |
| 复杂 OS 行为 | ✅ **独立 `.c` 文件** | 架构设计层面，逻辑复杂 |

> **核心原则**：`#ifdef` 用于**差异修补**；CMake + 独立文件用于**架构设计**。日志库、底层库、长期维护项目**永远选后者**。环形缓冲与原子后端虽属"算法/语法级"差异，但因平台 × 标准组合爆炸与测试隔离需要，同样按独立文件组织。

### 3.3 源码组织与平台分层（规范性）

```
include/hpulogc.h               公共 API（唯一对外头文件）
src/ring/ringbuf_locked.c       有锁环形缓冲（HPULOGC_LOCKFREE=OFF 时编译，此时 lockfree 版不参与编译）
src/ring/ringbuf_lockfree.c     无锁环形缓冲（HPULOGC_LOCKFREE=ON 时编译，此时有锁版不参与编译）
src/atomic/hpulogc_atomic.h     原子操作统一内联接口（按宏选择后端）
src/atomic/atomic_stdatomic.h   后端：C11 <stdatomic.h>（Linux / macOS）
src/atomic/atomic_gcc.h         后端：C99 __atomic_*（回退 __sync_*）（Linux / macOS）
src/atomic/atomic_msvc.h        后端：MSVC Interlocked*（Windows，全部语言标准）
src/platform/posix/…            POSIX 共享层：pthread 同步原语、时间、文件/目录（Linux 与 macOS 共用）
src/platform/linux/…            Linux 专属：inotify 热加载监视器等
src/platform/darwin/…           macOS 专属：kqueue/轮询回退监视器、pthread_threadid_np 等（Phase 3 实现）
src/platform/win32/…            Windows 专属：SRWLOCK/CONDITION_VARIABLE、路径与编码等（Phase 2 实现）
```

- **平台接口契约**（Phase 1 冻结，作为 win32/darwin 的移植依据）：同步原语（mutex/cond 封装）、线程封装、高精度时钟与本地时间转换、文件打开/写/fsync/轮转（rename/unlink/目录扫描语义）、目录创建与权限、路径处理、配置文件监视（watcher）、线程 ID 获取、动态库导出属性。
- CMake 按 `WIN32` / `APPLE` / 其余 选择对应平台目录的源文件参与编译；`src/platform/win32/`、`src/platform/darwin/` 在对应阶段之前仅含接口头文件与占位说明，不参与编译（见 §16）。
- 有锁环形缓冲本体保持单文件可移植：其互斥/条件变量通过平台层原语封装（`platform/posix/`、`platform/win32/` 各自实现同一契约接口）。
- **约束**：Phase 2 / Phase 3 期间，`src/` 中非平台目录（`ring/`、`atomic/` 统一接口、core、conf、format 等）与 `include/` 不得修改（"公共代码零修改"验收，见 §16）。

---

## 4. 核心功能需求

### 4.1 日志级别

7 个枚举值，按严重程度递增（定义见 §7.6）：

```
HPULOGC_LEVEL_TRACE(0) < HPULOGC_LEVEL_DEBUG(1) < HPULOGC_LEVEL_INFO(2) < HPULOGC_LEVEL_WARN(3)
    < HPULOGC_LEVEL_ERROR(4) < HPULOGC_LEVEL_FATAL(5) < HPULOGC_LEVEL_OFF(6)
```

- `HPULOGC_LEVEL_TRACE` ~ `HPULOGC_LEVEL_FATAL` 是合法的日志级别；`HPULOGC_LEVEL_OFF` **仅用于运行时过滤阈值**（`hpulogc_set_level(HPULOGC_LEVEL_OFF)` 关闭全部输出），作为写入 API 的 level 参数时静默丢弃。
- 每条日志携带级别标签（级别名大写：`TRACE`/`DEBUG`/`INFO`/`WARN`/`ERROR`/`FATAL`）。
- **运行时**可动态修改全局过滤阈值（`hpulogc_set_level`）；启用 Category 时可用 `hpulogc_set_level_for_category` 按 category 覆盖（**覆盖语义**：该 category 已设置级别时以其为准，未设置时使用全局级别）。
- **编译期**可通过宏定义 `HPULOGC_COMPILE_TIME_LEVEL` 移除低于阈值的代码（零开销抽象）：裁剪点仅在便捷宏 `HPULOGC_xxx` 的预处理分支上，低于阈值的宏展开为空语句；对直接调用 `hpulogc_log()` 的代码无效。允许取值为 `HPULOGC_LEVEL_TRACE` ~ `HPULOGC_LEVEL_FATAL`，另允许 `HPULOGC_LEVEL_OFF`（移除全部便捷宏日志）。

### 4.2 并发模型

| 特性 | 说明 |
|------|------|
| 线程安全 | 所有对外 API 均为线程安全（`hpulogc_shutdown` 除外，见 §7.5） |
| 并发模式 | 支持 **SPSC**（单生产者单消费者）和 **MPSC**（多生产者单消费者） |
| **模式选择** | **编译期通过 `HPULOGC_CONCURRENCY` 选择（SPSC / MPSC），不可运行时更改** |
| 批量提交 | 支持批量 IO 请求提交（Batching），合并多次写入为单次同步写入调用 |

### 4.3 环形缓冲队列

提供两种实现，**编译期通过 `HPULOGC_LOCKFREE` 选项选择，不可运行时更改**；两者**分别以独立源文件实现**（§3.3），CMake 条件编译二选一。

#### 有锁版本（`HPULOGC_LOCKFREE=OFF`，默认）

| 平台 | 互斥原语 | 条件变量 |
|------|----------|----------|
| Linux / macOS | `pthread_mutex_t`（经平台层封装） | `pthread_cond_t`（经平台层封装） |
| Windows | `CRITICAL_SECTION` 或 `SRWLOCK`（经平台层封装） | `CONDITION_VARIABLE` |

#### 无锁版本（`HPULOGC_LOCKFREE=ON`）

原子原语按 **平台 × 语言标准** 矩阵选择（与 §2.2 一致）：

| 平台 | C11 构建（`HPULOGC_C_STANDARD=11`） | C99 构建（`HPULOGC_C_STANDARD=99`） |
|------|--------------------------------------|--------------------------------------|
| Linux | `<stdatomic.h>` | GCC/Clang `__atomic_*`；不可用时回退 `__sync_*` |
| macOS | `<stdatomic.h>` | Apple Clang `__atomic_*`（禁止已废弃的 `OSAtomic*`） |
| Windows | MSVC `Interlocked*` | MSVC `Interlocked*` |

- Windows 上无论 C 标准，一律使用 MSVC `Interlocked*`（MinGW-w64 亦通过 `intrin.h` 使用同一族内建，保证 Windows 后端唯一）。
- 后端默认由 CMake 按 `HPULOGC_C_STANDARD` 与编译器特性探测自动选择（如 `__STDC_NO_ATOMICS__`、编译器版本检测）；可用 `HPULOGC_ATOMIC_BACKEND` 强制指定（§11）。
- Linux 与 macOS 的 C11、C99 两条路径**分别独立实现并分别测试**（附录 A 中双映射环形缓冲等实现技巧不影响该要求）。

- 缓冲区大小**仅在初始化时指定**（配置文件 `[buffer]` 或代码内配置），运行期不可更改，热加载忽略该项（§10.5）。init 时若 `buffer size < 2 × max log length`（含条目元数据余量），自动提升至 `max(4KB, 2 × max log length)` 并输出 stderr 警告（保证任何单条日志均可入队，消除 wait 策略死锁边界，见 §9）。
- **溢出策略**（init 时配置，可选集合受编译期约束）：

| 策略 | 行为 | 有锁版本 | 无锁版本 |
|------|------|----------|----------|
| **discard** | 丢弃**新**日志；`dropped` 计数 +1（`hpulogc_get_stats` 可见），写入 API 不返回错误 | ✅ | ✅ |
| **overwrite** | 覆盖最旧的未消费条目；`overwritten` 计数 +1 | ✅ | ✅ 仅 SPSC（无锁 MPSC 构建下不可用：多生产者协同推进队首实现风险高，配置为 overwrite 时启动失败） |
| **wait** | 阻塞生产者直到有空闲空间（保证不丢日志） | ✅ | ❌ 不可用，配置为 wait 时启动失败（值合法但构建不支持，见 §10.4） |

> 溢出策略可用集合由编译期构建（并发模式 × 锁实现）决定；"值合法但构建不支持"时启动失败（§10.4 fail-fast）。

> 注意：并发模式（`concurrency`）与是否无锁（`lockfree`）由编译期构建决定，**不可通过配置文件设置**。

### 4.4 I/O 模型

- **同步/异步模式由构建版本决定**（见 §5），不可运行时切换。
- **所有文件写入均为同步文件写入**：无论同步模式还是异步模式，日志落盘只使用标准同步文件 I/O 调用（`fwrite`/`write` 等）。异步模式中的"异步"仅指后台消费者线程的存在，**不引入任何 OS 级异步 I/O 设施（无 io_uring / POSIX AIO / IOCP）**。
- 异步模式下，后台消费者线程对取出的日志条目执行**同步文件写入**，不阻塞生产者线程。
- 批量提交机制：消费者线程攒批多条日志后一次性同步写出（运行时参数：`batch size`、`flush interval`，配置文件键名见 §10.3）。

### 4.5 配置系统

| 属性 | 描述 |
|------|------|
| 配置格式 | INI 风格（zlog conf 兼容词法，§10.1），解析器完全内置，无外部依赖 |
| 风格 | 词法与解析参考 zlog conf.c 的实现；支持 categories、rules、formats（§10） |
| 可扩展性 | 预留接口，后续支持 JSON/YAML |
| 校验 | 解析失败**拒绝启动**，返回 `HPULOGC_ERR_CONFIG` + 行号 |
| 热加载 | 编译期 `HPULOGC_ENABLE_HOT_RELOAD=ON` 时支持；Linux 优先 inotify，SIGHUP 触发需 `signal reload = true`（库安装 handler，默认关闭，§10.3）；Windows/macOS 及 inotify 不可用时，按 `hot reload interval`（秒）轮询文件 mtime/大小回退。热加载生效范围见 §10.5 |
| 多 Category | 编译期 `HPULOGC_ENABLE_CATEGORY=ON` 时支持 |

### 4.6 日志轮转

| 策略 | 说明 |
|------|------|
| 按大小 | 达到阈值触发 |
| 按时间 | 按 hour / day / week / month（时间桶边界跨越时触发） |
| 组合 | both（同时按大小和时间，任一条件满足即轮转） |
| 备份数量 | 可配置（0 = 不限制），超出自动清理最旧备份 |
| 命名模板 | 可配置，占位符：`{base}`（基础文件名）、`{timestamp}`（轮转发生时间，格式固定 `%Y%m%d_%H%M%S`，时区跟随 `global.timezone`）、`{index}`（按 base 单调递增序号，从 1 起、不补零）；默认 `{base}.{timestamp}.{index}.log`。模板必须包含 `{index}` 与 `{timestamp}` 至少其一，否则配置错误（§10.4）。仅使用 `{index}`（不含 `{timestamp}`）时等效纯序号风格（对比 zlog `#s`，见附录 A.1-11） |
| 轮转机制 | **活动文件始终为配置 `path`**：触发轮转时将当前活动文件重命名为模板生成的归档名（目标名冲突时 `{index}` 递增至可用名），随后新建活动文件继续写入。"最旧备份"判定：含 `{timestamp}` 的按时间戳排序，仅含 `{index}` 的按序号排序 |
| 时间桶边界 | day = 自然日；week = 周一 00:00 起始（ISO-8601）；month = 自然月；均跟随 `global.timezone` |
| 符号链接 | 可选创建 `.latest` 软链接指向当前活动文件；Windows 上忽略该配置并输出警告（符号链接需特权） |
| 检查时机 | 每条日志写出前检查（size 与当前时间桶），检查开销须为 O(1)（不得逐条 `stat`） |

### 4.7 输出目标

| 目标 | 特性 |
|------|------|
| 文件 | 路径、权限（配置键 `file perms`/`dir perms`，POSIX 语义；Windows 上忽略并输出警告）、轮转参数均可运行时配置；`[outputs]` 中定义的所有 file 输出在 init 时即打开 |
| 控制台 | stdout/stderr 可选，ANSI 彩色（编译期 `HPULOGC_ENABLE_COLOR` 控制，且仅对终端设备生效，重定向到文件/管道时自动关闭颜色；Windows 终端需启用 VT 处理，见 §16.2） |
| Socket | 参数骨架预留（host/port/protocol），**不注册可用输出**；配置文件中定义 socket 类型的 output 时**启动失败**，返回 `HPULOGC_ERR_CONFIG`（fail-fast，不静默忽略，见 §10.4） |

### 4.8 编译期裁剪体系

通过以下 CMake 选项精确控制：

| 选项 | 默认 | 功能 |
|------|------|------|
| `HPULOGC_ENABLE_ASYNC` | ON | 异步写入（后台消费者线程）+ 批量提交 |
| `HPULOGC_ENABLE_COLOR` | ON | ANSI 彩色输出 |
| `HPULOGC_ENABLE_ROTATE` | ON | 日志轮转 |
| `HPULOGC_ENABLE_INI` | ON | INI 配置解析器 |
| `HPULOGC_ENABLE_HOT_RELOAD` | ON | 配置热加载 |
| `HPULOGC_ENABLE_CATEGORY` | ON | 多 Category 路由与 per-category 级别 |
| `HPULOGC_ENABLE_THROTTLE` | OFF | 限流与采样 |
| `HPULOGC_ENABLE_SOURCE_LOC` | ON | `__FILE__`/`__LINE__` 捕获 |
| `HPULOGC_LOCKFREE` | OFF | 无锁队列（选择 `ringbuf_lockfree.c` / `ringbuf_locked.c`，§3.3） |
| `HPULOGC_CONCURRENCY` | MPSC | SPSC / MPSC |
| `HPULOGC_COMPILE_TIME_LEVEL` | TRACE（即不裁剪） | 编译期移除低于该级别的日志代码（仅作用于便捷宏，见 §4.1）；允许取值 `HPULOGC_LEVEL_TRACE` ~ `HPULOGC_LEVEL_FATAL` 及 `HPULOGC_LEVEL_OFF` |

> **被裁剪功能的 API 行为**：对外 API 符号仍保留（stub 实现），调用时返回 `HPULOGC_ERR_CONFIG`（如 `HPULOGC_ENABLE_CATEGORY=OFF` 时的 `hpulogc_set_level_for_category`），不产生未定义行为。
>
> **`HPULOGC_ENABLE_CATEGORY=OFF` 时的路由语义**：所有路由规则按 `*` 匹配处理（忽略规则中 category 选择器的具体内容，仅 level 范围与 format/outputs 生效）；`%category` 展开为空串。

### 4.9 日志处理管线（规范性）

一条日志从调用到落盘的处理顺序**固定**如下；各步骤的执行线程由同步/异步模式决定：

```
HPULOGC_xxx 宏 / hpulogc_log()
  ① 编译期裁剪        （HPULOGC_COMPILE_TIME_LEVEL，低于阈值的宏展开为空，直接调用不受限）
  ② 级别过滤          （per-category 级别覆盖全局级别：该 category 已设置级别时以其为准，
                       未设置时用全局级别；低于阈值静默丢弃）
  ③ 限流/采样         （HPULOGC_ENABLE_THROTTLE=ON 时：先限流后采样；超限丢弃并计入 throttled）
  ④ 消息体渲染 + 入队  （%msg 展开与缓冲区满时按 overflow_policy 处理；消息体渲染长度上限为
                       max log length，见下与 §9）
  ⑤ 取出              （同步模式：调用线程直接继续；异步模式：单一消费者线程批量取出）
  ⑥ 路由              （rules 从上到下首条命中，确定 format 与 outputs；无命中规则时按
                       default format + default outputs 兜底输出，default outputs 为空则丢弃）
  ⑦ 整行格式化        （按命中规则的 format 生成完整日志行；json 格式仅执行 JSON 转义、
                       跳过注入转义，见 §12）
  ⑧ 输出              （写入该规则的所有 outputs，按 crash_safety 策略 fsync）
```

- **同步模式**（`HPULOGC_ENABLE_ASYNC=OFF` 或构建预设 min/sync_thread）：②-⑧ 全部在调用者线程执行，无后台线程。
- **异步模式**：②③④（含**消息体渲染**）在生产者线程，⑤⑥⑦⑧ 在唯一消费者线程。消息体渲染指 `%msg` 的展开（`fmt` + `va_list`）——`va_list` 不可跨线程，**必须**在生产者线程完成；渲染结果长度上限为 `max log length`（超出部分静默丢弃，不加 marker；整行截断与 `truncation marker` 在步骤⑦整行层面附加，§9）。入队载荷为"消息体（受 `max log length` 约束）+ 元数据（level、category、file、line、func、时间戳）"。消费者线程完成路由（⑥）、整行组装（⑦，将消息体嵌入格式模板并渲染其余占位符）与输出（⑧），并通过**同步文件写入**完成落盘。
- **格式预编译（规范性）**：格式模板与路由规则在 init/热加载时预编译为内部动作序列（含占位符类型解析）；热路径不得对格式模板做字符串二次解析。
- **注入防护（`escape injection`）在步骤 ⑦ 执行**：仅转义消息体（`%msg` 展开结果）中的换行符（`\n`/`\r` → `\n` 字面转义文本）和 ANSI 转义序列（`ESC` → `\x1b` 字面文本）；不影响库自身生成的格式与颜色序列。json 格式例外（§12）。
- **category 语义**：无需注册，写入时直接传字符串；库内部按需登记（用于 per-category 级别缓存与路由加速），登记表容量为 `HPULOGC_MAX_CATEGORIES`（默认 64，编译期可覆盖），表满后新 category 按 `*` 兜底规则路由并输出一次 stderr 警告（每个被拒绝的 category 仅一次）。category 为 NULL 或空串等价于类别名 `"*"`（仅匹配选择器 `*`，`%category` 展开为空串）。category 名为任意非空字节串，`.` 保留为层级分隔符，其余字符按字面匹配（库不做字符集校验，避免热路径开销）。

---

## 5. 构建版本预设

| 版本 | 强制 CMake 选项（覆盖默认值） | 可用配置节 | 适用场景 |
|------|------------------------------|-----------|----------|
| **full** | 无强制（全部功能 ON，`CONCURRENCY=MPSC`，`LOCKFREE=OFF`） | 全部节 | 通用服务端 |
| **min** | `ASYNC=OFF`、`ROTATE=OFF`、`COLOR=OFF`、`CATEGORY=OFF`、`HOT_RELOAD=OFF`、`THROTTLE=OFF`、`INI=OFF`、`CONCURRENCY=SPSC`、`LOCKFREE=OFF` | 无（INI=OFF，不解析配置文件）：仅代码内配置 `hpulogc_config_t` | 嵌入式/资源受限 |
| **sync_thread** | `ASYNC=OFF`、`LOCKFREE=OFF`（其余默认，`CONCURRENCY=MPSC`） | global, formats, outputs, buffer(mpsc), rules, advanced（[async] 节跳过） | 单线程/低并发 |
| **async_single** | `ASYNC=ON`、`LOCKFREE=ON`、`CONCURRENCY=SPSC` | global, formats, outputs, buffer(无锁/spsc), async, rules, advanced | 高吞吐生产 |

> **预设与用户 CMake 选项冲突时，以预设为准**，并在 CMake 配置期输出警告。预设仅服务于其定位场景：需要"资源受限 + 配置文件"组合的用户可不使用预设，直接以 `HPULOGC_ENABLE_*` 选项自行组合。
>
> **min 版本可配置项**（代码内）：`escape_injection`、`max_log_length`、`truncation_marker`、`crash_safety`、`signal_safe`、`fork behavior` 等高级项保留；stats 相关项在 min 下忽略（无后台线程）。
>
> **各预设可用溢出策略**受 §4.3 构建约束：如 async_single（无锁 SPSC）支持 discard/overwrite；无锁 MPSC 组合仅支持 discard。
>
> **min 版本体积验证条件**：Linux x86_64，GCC `-Os`，strip 后 `.text` 段 ≤ 8 KB（INI 解析器已随 INI=OFF 排除在 min 之外）。

---

## 6. 依赖与体积

| 要求 | 说明 |
|------|------|
| 语言依赖 | 仅标准 C 库（`libc`） |
| OS 依赖 | 仅 OS 原生 API |
| 第三方库 | **零依赖**；INI 解析器完全自包含（词法参考 zlog conf.c，自研实现） |
| 最小体积 | `min` 版本代码段 ≤ 8KB（验证条件见 §5） |
| 头文件 | 公共 API 集中在 `hpulogc.h`，内部头文件不对外暴露 |

---

## 7. API 与接口设计

### 7.1 命名与导出

- 所有 API 使用 `hpulogc_` 前缀。
- 导出宏 `HPULOGC_API`：Windows → `__declspec(dllexport/dllimport)`；GCC/Clang → `__attribute__((visibility("default")))`。
- 版本宏：`HPULOGC_VERSION_MAJOR` / `HPULOGC_VERSION_MINOR` / `HPULOGC_VERSION_PATCH`。

### 7.2 初始化方式

| 方式 | API |
|------|-----|
| 配置文件路径 | `int hpulogc_init_from_file(const char* path)` |
| 代码内配置 | `int hpulogc_init(const hpulogc_config_t* cfg)` |
| 默认配置 | `int hpulogc_init_default(void)` |
| 填充默认配置结构 | `void hpulogc_config_default(hpulogc_config_t* cfg)` |
| 销毁 | `void hpulogc_shutdown(void)` |

- `hpulogc_init(NULL)` 等价 `hpulogc_init_default()`；`hpulogc_init_from_file(NULL)` 视为参数非法，返回 `HPULOGC_ERR_INVALID_ARG`。
- `hpulogc_config_default(cfg)` 以全部默认值填充 `hpulogc_config_t`（cfg 为 NULL 时安全忽略）。**调用方必须先调用本函数初始化结构体（或对全部字段显式赋值）后再调用 `hpulogc_init`；未经初始化的字段内容未定义**。
- 默认配置（`hpulogc_init_default()`）语义：level=INFO、standard 格式、内置 stderr 控制台输出、buffer 1MB、溢出 discard。
- 代码内配置与配置文件的能力边界：**代码内配置为配置文件的功能子集**。`hpulogc_config_t` 覆盖核心运行时项（见 §7.6）；以下键仅可通过配置文件设置——`timezone`、`timestamp source`、`time format`、`encoding`、`newline`、`pid format`/`tid format`、`capture source loc`、`hot reload interval`、`signal reload`、`strict init`、`[throttle]` 全部参数、`[advanced]` stats 项。**命名 formats 表与多路由规则只能通过 `hpulogc_rule_t` 数组表达，rule 的 `format` 字段仅允许引用 5 个内置格式名**（`minimal`/`standard`/`categorized`/`detailed`/`json`）；自定义命名格式模板仅可通过配置文件定义。
- **代码内配置的值非法**（范围外、非法枚举值、引用未定义名称、rule 的 min_level > max_level 等）→ init 返回 `HPULOGC_ERR_INVALID_ARG` 或 `HPULOGC_ERR_CONFIG`，**不做 clamp**（clamp + 警告仅适用于配置文件路径，§10.4）。

### 7.3 核心 API 签名

```c
/* 初始化与销毁 */
int  hpulogc_init(const hpulogc_config_t* cfg);
int  hpulogc_init_from_file(const char* config_path);
int  hpulogc_init_default(void);
void hpulogc_config_default(hpulogc_config_t* cfg);  /* 填充默认值；cfg 为 NULL 安全忽略 */
void hpulogc_shutdown(void);

/* 强制刷新：flush 将队列中已入队日志尽量写出（异步模式通知消费者立即提交，同步模式刷新 stdio），不 fsync；
 * sync = flush + 对所有 file 输出强制 fsync。
 * 未初始化时返回 HPULOGC_ERR_STATE。 */
int  hpulogc_flush(void);
int  hpulogc_sync(void);

/* 运行时动态控制（未初始化时返回 HPULOGC_ERR_STATE）。
 * set_level 接受 HPULOGC_LEVEL_OFF 关闭全部输出；
 * set_level_for_category 覆盖全局级别（覆盖语义：该 category 已设置级别时以其为准），
 * category 为 NULL 时返回 HPULOGC_ERR_INVALID_ARG。 */
int  hpulogc_set_level(hpulogc_level_t level);
int  hpulogc_set_level_for_category(const char* category, hpulogc_level_t level);

/* 构建信息查询：info 为 NULL 时安全忽略；可在任意时刻调用（不要求已初始化）。 */
typedef struct {
    const char* build_version;       /* "full" / "min" / "sync_thread" / "async_single" */
    int         has_async;
    int         has_color;
    int         has_rotate;
    int         has_hot_reload;
    int         has_category;
    int         has_throttle;
    int         has_ini;
    int         lockfree;
    const char* concurrency;         /* "spsc" / "mpsc" */
} hpulogc_build_info_t;
void hpulogc_get_build_info(hpulogc_build_info_t* info);

/* 运行统计（线程安全，原子读取；未初始化时返回 HPULOGC_ERR_STATE，stats 为 NULL 返回 HPULOGC_ERR_INVALID_ARG） */
typedef struct {
    unsigned long long accepted;     /* 通过过滤进入队列的条数 */
    unsigned long long dropped;      /* discard 溢出丢弃条数 + 运行期写失败丢弃条数 */
    unsigned long long overwritten;  /* overwrite 策略覆盖的条数 */
    unsigned long long throttled;    /* 限流/采样丢弃条数（THROTTLE=OFF 时恒为 0） */
    unsigned long long written;      /* 已写出到 output 的条数（按条计数：一条日志输出至多个 output 仍计 1） */
    size_t             buffer_used;  /* 当前缓冲区占用字节 */
    size_t             buffer_size;  /* 缓冲区总大小字节 */
} hpulogc_stats_t;
int  hpulogc_get_stats(hpulogc_stats_t* stats);

/* 日志写入（未初始化时静默丢弃；level 为 HPULOGC_LEVEL_OFF 或非法时静默丢弃） */
void hpulogc_log(hpulogc_level_t level, const char* category,
                 const char* file, int line, const char* func,
                 const char* fmt, ...);
void hpulogc_vlog(hpulogc_level_t level, const char* category,
                  const char* file, int line, const char* func,
                  const char* fmt, va_list ap);

/* async-signal-safe 受限写入（本函数始终为 async-signal-safe，并保存/恢复调用线程的 errno）。
 * signal_safe = false（默认）时调用本函数静默丢弃（无未定义行为）；
 * signal_safe = true 时库在 init 时预分配信号通道资源。
 * 无锁、无格式化、无内存分配，通过 write() 直写 stderr；
 * msg 必须为调用方提供的以 '\0' 结尾的静态/栈上缓冲区。 */
void hpulogc_log_signal_safe(hpulogc_level_t level, const char* msg);

/* 便捷宏（HPULOGC_COMPILE_TIME_LEVEL 裁剪点） */
#define HPULOGC_TRACE(cat, fmt, ...)  hpulogc_log(HPULOGC_LEVEL_TRACE, cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define HPULOGC_DEBUG(cat, fmt, ...)  hpulogc_log(HPULOGC_LEVEL_DEBUG, cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define HPULOGC_INFO(cat, fmt, ...)   hpulogc_log(HPULOGC_LEVEL_INFO,  cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define HPULOGC_WARN(cat, fmt, ...)   hpulogc_log(HPULOGC_LEVEL_WARN,  cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define HPULOGC_ERROR(cat, fmt, ...)  hpulogc_log(HPULOGC_LEVEL_ERROR, cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define HPULOGC_FATAL(cat, fmt, ...)  hpulogc_log(HPULOGC_LEVEL_FATAL, cat, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
```

> **可移植性说明（`##__VA_ARGS__`）**：头文件按以下顺序自动选择宏实现，并以 `HPULOGC_HAS_VA_ARGS` 标识当前实现：
> 1. GCC/Clang：`fmt, ##__VA_ARGS__`（GNU 扩展，允许零个可变参数）；
> 2. MSVC 传统预处理器（`_MSC_VER` 定义且 `_MSVC_TRADITIONAL` 未定义或 `!= 0`）：`fmt, __VA_ARGS__`（依赖 MSVC 对空 `__VA_ARGS__` 的自动吞逗号行为）；
> 3. MSVC `/Zc:preprocessor`（`_MSVC_TRADITIONAL == 0`）或 `__VA_OPT__` 可用（C23/C++20）：`fmt __VA_OPT__(,) __VA_ARGS__`；
> 4. 其余严格 C99 环境：`fmt, __VA_ARGS__`（要求调用时至少提供一个可变参数）。

### 7.4 错误处理

- 初始化/配置 API 返回 `int`（`0` = 成功，负值 = 错误类型）。
- 日志写入 API 返回 `void`，**不依赖 `errno`**（且不得破坏调用者的 `errno`，见 §9），**不使用 `longjmp`**，丢弃类失败通过 `hpulogc_get_stats` 计数暴露。
- 错误类型：
  - `HPULOGC_ERR_INVALID_ARG`：参数非法（NULL、越界枚举值、代码内配置值超出合法范围等）
  - `HPULOGC_ERR_NO_MEM`：内存分配失败
  - `HPULOGC_ERR_IO`：文件/设备 I/O 失败（init 时输出打开失败等）
  - `HPULOGC_ERR_CONFIG`：配置缺失、非法、值合法但被当前构建裁剪
  - `HPULOGC_ERR_STATE`：状态错误（未初始化、重复初始化、已 shutdown 后调用）
- **诊断信息**：init/配置错误的详情（配置文件名、行号、失败原因）统一输出至 stderr；本版本不提供错误字符串查询 API（`hpulogc_strerror()` 列入 P2，§17）。

### 7.5 生命周期语义

| 场景 | 行为 |
|------|------|
| 未初始化时调用写入 API（`hpulogc_log` 等） | 静默丢弃，不崩溃、不输出 |
| 未初始化时调用控制 API（`set_level`/`flush`/`sync`/`get_stats`） | 返回 `HPULOGC_ERR_STATE` |
| 重复调用 init（未 shutdown 再次 init） | 返回 `HPULOGC_ERR_STATE`，已有实例不受影响 |
| init 失败 | 库保持未初始化状态，已打开的资源全部释放，可安全重试 init |
| init 与其他 API 并发 | **不承诺线程安全**：调用方须保证 init 完成后其他线程才可使用日志 API |
| shutdown 重复调用 | 幂等，安全返回 |
| shutdown 内部行为 | 先 flush 并等待队列排空（至多 `shutdown_timeout_ms`），然后关闭所有输出、释放资源 |
| shutdown 之后调用写入 API | 安全返回/丢弃，不崩溃 |
| shutdown 与其他 API 并发 | **不承诺线程安全**，调用方需保证 shutdown 与日志写入不并发 |

### 7.6 公共数据结构定义（规范性）

```c
/* ---- 日志级别（见 §4.1） ---- */
typedef enum {
    HPULOGC_LEVEL_TRACE = 0,
    HPULOGC_LEVEL_DEBUG = 1,
    HPULOGC_LEVEL_INFO  = 2,
    HPULOGC_LEVEL_WARN  = 3,
    HPULOGC_LEVEL_ERROR = 4,
    HPULOGC_LEVEL_FATAL = 5,
    HPULOGC_LEVEL_OFF   = 6    /* 仅用于过滤阈值，不是合法的日志级别 */
} hpulogc_level_t;

/* ---- 容量上限（编译期可通过 -D 覆盖） ---- */
#define HPULOGC_MAX_OUTPUTS      16   /* 代码内配置 output 数组上限；配置文件同此上限 */
#define HPULOGC_MAX_RULES        64   /* 代码内配置 rule 数组上限；配置文件同此上限 */
#define HPULOGC_MAX_CATEGORIES   64   /* 内部 category 登记表容量 */
#define HPULOGC_MAX_NAME_LEN     64   /* format/output/category 名称长度 */
#define HPULOGC_MAX_PATH_LEN     512  /* 文件路径长度 */
#define HPULOGC_MAX_FMT_LEN      256  /* 格式模板/配置字符串值长度 */

/* ---- 枚举 ---- */
typedef enum {
    HPULOGC_OUT_CONSOLE = 0,
    HPULOGC_OUT_FILE    = 1
} hpulogc_output_type_t;

typedef enum {
    HPULOGC_ROTATE_NONE = 0,
    HPULOGC_ROTATE_SIZE = 1,
    HPULOGC_ROTATE_TIME = 2,
    HPULOGC_ROTATE_BOTH = 3
} hpulogc_rotate_t;

typedef enum {
    HPULOGC_TU_HOUR = 0, HPULOGC_TU_DAY = 1,
    HPULOGC_TU_WEEK = 2, HPULOGC_TU_MONTH = 3
} hpulogc_time_unit_t;

typedef enum {
    HPULOGC_OVERFLOW_DISCARD   = 0,
    HPULOGC_OVERFLOW_OVERWRITE = 1,
    HPULOGC_OVERFLOW_WAIT      = 2
} hpulogc_overflow_policy_t;

typedef enum {
    HPULOGC_TS_REALTIME = 0,   /* CLOCK_REALTIME / 系统挂钟 */
    HPULOGC_TS_MONOTONIC = 1   /* 单调时钟，相对 init 的秒.微秒 */
} hpulogc_timestamp_source_t;

typedef enum {
    HPULOGC_NEWLINE_AUTO = 0,  /* Unix=\n / Windows=\r\n */
    HPULOGC_NEWLINE_LF   = 1,
    HPULOGC_NEWLINE_CRLF = 2
} hpulogc_newline_t;

typedef enum {
    HPULOGC_CRASH_NONE     = 0,  /* 不 fsync */
    HPULOGC_CRASH_PERIODIC = 1,  /* 消费者按 flush_interval 周期 fsync（异步）；同步模式等效 shutdown */
    HPULOGC_CRASH_ENTRY    = 2,  /* 每条日志 fsync */
    HPULOGC_CRASH_SHUTDOWN = 3   /* 仅 shutdown 时 fsync */
} hpulogc_crash_safety_t;

/* 注意：crash_safety 枚举值顺序 ≠ fsync 严格度顺序；
 * 严格度排序为 none < shutdown < periodic < entry（§9 与 per-output fsync 取更严格者）。 */

/* ---- 输出目标（代码内配置） ---- */
typedef struct {
    hpulogc_output_type_t type;
    int          stream;            /* console: 0=stdout, 1=stderr */
    int          color;             /* console: 是否彩色 */
    const char*  path;              /* file: 日志文件路径 */
    hpulogc_rotate_t      rotate;
    size_t               max_size;  /* bytes，rotate 含 size 时有效 */
    hpulogc_time_unit_t   time_unit; /* rotate 含 time 时有效 */
    int          max_files;         /* 0 = 不限制 */
    int          fsync;             /* 每条日志后 fsync（与全局 crash_safety 取更严格者，§9） */
    int          symlink_latest;    /* Windows 忽略 */
    const char*  rotate_naming;     /* NULL = "{base}.{timestamp}.{index}.log" */
    unsigned     file_mode;         /* POSIX 八进制权限（0644）；Windows 忽略 */
    unsigned     dir_mode;          /* 同上（0755） */
} hpulogc_output_t;

/* ---- 路由规则（代码内配置；format 仅允许内置格式名） ---- */
typedef struct {
    const char*    category;         /* 精确名 / "name.*" / "*"；NULL/空串等价 "*" */
    hpulogc_level_t min_level;        /* 闭区间；min_level > max_level → 配置错误 */
    hpulogc_level_t max_level;        /* 闭区间 */
    const char*    format;           /* "minimal"/"standard"/"categorized"/"detailed"/"json" */
    const char* const* outputs;      /* output 名称数组 */
    size_t         output_count;
} hpulogc_rule_t;

/* ---- 代码内配置（公开 POD；须先经 hpulogc_config_default() 初始化，见 §7.2/§7.3） ---- */
typedef struct {
    /* 级别与格式 */
    hpulogc_level_t       level;            /* 全局过滤阈值，默认 INFO */
    const char*          default_format;   /* 未命中规则的兜底 format 名；NULL = "standard" */
    const char* const*   default_outputs;  /* 兜底 outputs 名称数组；NULL/空 = 未命中规则时丢弃 */
    size_t               default_output_count;
    /* 输出与路由 */
    const hpulogc_output_t* outputs;  size_t output_count;  /* 数组由调用方持有，init 期间拷贝所需元数据 */
    const hpulogc_rule_t*   rules;    size_t rule_count;
    /* 缓冲区 */
    size_t                       buffer_size;       /* 字节，默认 1MB，范围 4KB~1GB；< 2×max_log_length 时 init 自动提升 + 警告 */
    hpulogc_overflow_policy_t     overflow_policy;   /* 默认 discard */
    /* 异步（构建 ASYNC=OFF 时以下三项忽略） */
    uint32_t             batch_size;          /* 默认 64 */
    uint32_t             flush_interval_ms;   /* 默认 100 */
    uint32_t             shutdown_timeout_ms; /* 默认 5000 */
    /* 高级 */
    int                  escape_injection;    /* 默认 1 */
    size_t               max_log_length;      /* 默认 4096，范围 256~65536 */
    const char*          truncation_marker;   /* 默认 "...[TRUNCATED]" */
    hpulogc_crash_safety_t crash_safety;       /* 默认 shutdown */
    int                  signal_safe;         /* 默认 0 */
} hpulogc_config_t;
```

> 各结构体字段布局实现时可微调，但**语义、默认值、取值范围以本节为准**；同 Major 版本内 POD 结构只允许尾部追加字段。
>
> 代码内配置值非法 → init 返回 `HPULOGC_ERR_INVALID_ARG`（不 clamp，区别于配置文件的 clamp + 警告，§10.4）。

---

## 8. 性能目标

| 指标 | 目标值 | 条件 |
|------|--------|------|
| 单条延迟（无竞争） | < 100 ns | SPSC + 无锁 + 64B 消息 |
| P99 延迟（MPSC 竞争，无锁） | < 1 μs | MPSC + **无锁** + 8 生产者 |
| P99 延迟（MPSC 竞争，有锁） | < 10 μs | MPSC + **有锁** + 8 生产者 |
| 吞吐量（峰值） | > 500,000 logs/sec | 64B/条，异步模式，消费者线程批量同步写 |
| 基线内存 | < 32 KB（不含缓冲区） | `min` 版本 |
| 缓冲区范围 | 4 KB ~ 1 GB（可配置） | 默认 1 MB |

- CI 中纳入性能基准线，吞吐量下降 > 10% 触发告警。

---

## 9. 安全性与健壮性

| 类别 | 要求 |
|------|------|
| 日志注入防护 | 在格式化阶段仅转义**用户消息（`%msg`）**中的换行符和 ANSI 转义序列（可配置开关）；不影响库自身生成的颜色/格式序列；json 格式仅执行 JSON 转义、跳过注入转义（§12） |
| 格式化安全 | API 强制 `fmt` 与参数分离，禁止用户字符串作格式串 |
| 单条长度限制 | `max log length` 作用于**格式化后的完整日志行**（含换行），默认 4 KB、范围 256~65536 字节；超出截断并附加 `truncation marker`，截断后总长（含 marker）仍不超过 `max log length`。**生产者线程的消息体渲染上限同为 `max log length`**（超出部分静默丢弃、不加 marker；整行截断与 marker 在格式化阶段执行，§4.9④）。init 校验 `buffer size ≥ 2 × max log length`（含元数据余量），不足时自动提升并警告（§4.3），保证 wait 策略不存在"单条日志永不入队"的死锁边界 |
| 信号处理 | 普通 API 不可在 signal handler 中调用；`hpulogc_log_signal_safe()` **始终为 async-signal-safe** 并保存/恢复 `errno`；`signal safe = false`（默认）时调用该函数静默丢弃（无未定义行为），`true` 时 init 预分配信号通道资源（见 §7.3），文档明确其受限子集 |
| fork() 安全 | `pthread_atfork` 处理（Windows 无 fork，该配置忽略），子进程行为由 `fork behavior` 配置：**reinit**（child handler 仅设置原子脏标记——async-signal-safe；子进程首次调用任意需要运行态的日志 API 时惰性重建：文件型配置重读配置文件，代码内配置使用 init 时保存的副本；重建失败该条丢弃并限频警告）/ **disable**（子进程中日志调用静默丢弃）/ **inherit**（继承父进程内部状态，不安全，仅调试） |
| 崩溃安全 | `crash safety` 为 fsync 策略：none / periodic（周期 = `flush interval`）/ entry（每条）/ shutdown；**fsync 严格度排序：none < shutdown < periodic < entry**；有效策略 = 全局 `crash_safety` 与各 output 的 `fsync` 配置**取更严格者**（output `fsync=true` ⇒ 该输出至少 entry 级） |
| errno 保持 | 正常路径与错误路径的日志写入 API（含 `hpulogc_log_signal_safe`）**保存并恢复调用线程的 `errno`** |
| 并发正确性 | 所有跨线程共享的计数与状态标志（`dropped`/`overwritten`/`throttled`/`accepted`/`written`、periodic fsync 的周期计数、shutdown 状态等）必须通过 §2.2 原子后端实现，**禁止依赖普通整型的"事实原子性"** |
| 多进程边界 | 仅承诺**单进程内多线程**安全；不承诺多进程写同一日志文件及并发轮转安全（如需跨进程请外部协调，见附录 A.2-4） |
| 文件 I/O 失败 | **init 时**：任一 file output 打开失败 → 启动失败，返回 `HPULOGC_ERR_IO`（fail-fast）；**运行期**：写入失败时尝试重新打开文件一次，仍失败则该条丢弃（计入 `dropped`）并输出限频 stderr 警告（每 5 秒至多 1 条），后续每条日志重试重开。磁盘满/只读遵循同一策略 |
| 内存安全 | 初始化时预分配；稳态（日志写入热路径）不调用 `malloc`/`free`；允许的例外仅限：首次登记新 category、per-thread 渲染缓冲首次使用及按需增长（上限 `max log length`） |
| 字符串安全 | 所有操作使用 `snprintf` 等长度受限函数 |

---

## 10. 配置文件规范

### 10.1 设计原则

1. **zlog 词法基线（规范性）**：配置文件词法与解析器实现参考 zlog（HardySimpson/zlog）`conf.c` 的实现：**自研行式解析器**（无外部 INI 库）、仅 `#` 注释、引号感知、反斜杠续行、解析错误必须带文件名与行号。节集合与 rules 语义为 hpulogc 扩展；与 zlog 的其余差异记录于附录 A。
2. **词法规则（规范性）**：
   - 键值以**第一个 `=`** 分割；value 为 `=` 之后至行尾的内容（去首尾空白）；若 value 以成对双引号包裹则去引号，**引号内的 `#`、`,`、`=` 均不具特殊含义**（不作注释、不作行内分隔符，允许含空格的值，如 `time format`、`truncation marker`）；
   - 行首（首个非空白字符）为 `#` 的行是注释行；行尾 `#` 及其后内容视为注释（引号外）；`;` **不是**注释字符；
   - 行尾独立反斜杠 `\` 为续行符，下一行内容拼接至当前行后再解析（错误行号按首行计）；
   - **键名区分大小写**，采用 zlog 风格空格分词（如 `strict init`、`buffer size`）；级别名、布尔值、枚举**值**大小写不敏感；
   - 尺寸后缀（不区分大小写，zlog 语义）：`1k`=1000、`1kb`=1024、`1m`=10⁶、`1mb`=2²⁰、`1g`=10⁹、`1gb`=2³⁰；无后缀为字节数；
   - `[outputs]` 行内参数为逗号分隔的 `key=value`，值可用双引号包裹（含空格、`,`、`=` 的值）；
   - 物理行长度 ≤ 1024 字节（续行各段分别计），超出启动失败。
   - 配置文件键名与 §7.6 代码内配置字段一一对应（如 `buffer size` ↔ `buffer_size`）；代码内配置不受配置文件词法约束。
3. **配置文件中只包含运行时可配置项**。
4. **编译期决策通过 `[build]` 节只读展示**，用户不可通过配置文件更改。
5. 被裁剪功能的配置节在解析时**静默跳过**；裁剪节内部的单个键亦静默忽略。
6. 配置项值**超出合法范围**时，裁剪到最近有效值 + stderr 警告。
7. 配置项值**合法但当前构建不支持**时（fail-fast），**启动失败**并返回 `HPULOGC_ERR_CONFIG`，不做静默降级（见 §10.4）。
8. 长度与数量限制：配置字符串值 ≤ `HPULOGC_MAX_FMT_LEN`（256 字节），路径 ≤ `HPULOGC_MAX_PATH_LEN`（512 字节），outputs 数量 ≤ `HPULOGC_MAX_OUTPUTS`（16），rules 条数 ≤ `HPULOGC_MAX_RULES`（64），超出启动失败（`HPULOGC_ERR_CONFIG`）。

### 10.2 节结构总览

> **节出现顺序（规范性，zlog 顺序校验语义）**：**出现的节**必须遵循下表自上而下的相对顺序（`[build]` → `[global]` → `[formats]` → `[outputs]` → `[buffer]` → `[async]` → `[throttle]` → `[rules]` → `[advanced]`）；顺序回退或出现未知节名 → 启动失败（`HPULOGC_ERR_CONFIG`），报行号。
>
> **所有节均可省略**（省略 = 使用该节全部键的默认值）；空文件或仅含注释/空行的配置文件合法。`[build]` 为可选节（存在时忽略其值，见 §10.4）。
>
> 配置文件未定义任何 output 且未设置 `default outputs` 时，输出一次 stderr 提示（避免全静默误配）。

| 节名 | 可读性 | 内容 | 裁剪行为 |
|------|--------|------|----------|
| `[build]` | 只读 | 构建版本、并发模式、功能可用性 | 可选；始终解析（值被忽略） |
| `[global]` | 读写 | 语法严格性、级别、兜底 format/outputs、时间、编码、换行、信号触发 | 始终解析 |
| `[formats]` | 读写 | 命名格式模板 | 始终解析 |
| `[outputs]` | 读写 | 输出目标定义 | 始终解析（裁剪功能参数静默忽略） |
| `[buffer]` | 读写 | 缓冲区大小、溢出策略 | 始终解析 |
| `[async]` | 读写 | 批量/刷新参数 | `HPULOGC_ENABLE_ASYNC=OFF` 时跳过 |
| `[throttle]` | 读写 | 限流/采样参数 | `HPULOGC_ENABLE_THROTTLE=OFF` 时跳过 |
| `[rules]` | 读写 | 路由规则 | 始终解析 |
| `[advanced]` | 读写 | 安全/健壮性/统计 | 始终解析 |

### 10.3 完整配置模板

```ini
# ============================================================================
# hpulogc 日志配置文件
# ============================================================================
# 语法：INI 风格，词法与 zlog conf 一致（§10.1）。
#   - 仅 # 为注释字符：# 起始的行为注释行；行尾 # 后为注释（双引号内除外）。
#   - 行尾独立反斜杠 \ 为续行符。
#   - key = value；value 为 = 后整行（可含空格），可用成对双引号包裹；
#     引号内 # 、 , 、= 均不具特殊含义。
#   - 键名区分大小写（空格分词）；级别名/布尔/枚举值大小写不敏感。
#   - 尺寸后缀：1k=1000, 1kb=1024, 1m=10^6, 1mb=2^20, 1g=10^9, 1gb=2^30。
#   - 出现的节须保持相对顺序（§10.2）；所有节均可省略（省略 = 默认值）。
# ============================================================================


# ============================================================================
# [build] 构建信息（可选；只读展示，值与实际构建不一致时忽略并警告）
# ============================================================================
[build]

build version = full
concurrency = mpsc
lockfree = false
has async = true
has color = true
has rotate = true
has hot reload = true
has category = true
has throttle = false
has ini = true


# ============================================================================
# [global] 全局运行时配置
# ============================================================================
[global]

# 语法严格性：true 时未知键启动失败；false 时未知键降级为 stderr 警告并忽略
# （其余错误——语法错误、缺值、重复定义、引用未定义名称——无论取值均启动失败）
# 仅在 init 时生效；热加载解析时未知键按当前生效值处理（§10.5）
strict init = true

# 过滤级别，低于此级别的日志被丢弃：TRACE ~ FATAL
level = INFO

# 未命中任何规则时的兜底 format 名（引用 [formats] 中已定义的名称）
default format = standard

# 未命中任何规则时的兜底 outputs（逗号分隔，引用 [outputs] 中已定义的名称）
# 未命中规则且该值为空时，该条日志被丢弃
default outputs = console_out

# 时区：local（本地时区）或 utc
timezone = local

# 时间戳源：
#   realtime  - CLOCK_REALTIME，真实时间，受 time format 控制显示格式
#   monotonic - 单调时钟，%time 固定输出相对 init 的 "秒.微秒"，time format 忽略
timestamp source = realtime

# 时间格式（strftime 风格，扩展占位符：%f 微秒、%F3 毫秒）
time format = %Y-%m-%d %H:%M:%S.%f

# 字符编码（当前仅支持 utf-8）
encoding = utf-8

# 运行时是否捕获源码位置（编译期 HPULOGC_ENABLE_SOURCE_LOC=OFF 时此键忽略）
capture source loc = true

# 换行符：auto（跟随平台，Unix=\n / Windows=\r\n）、lf（统一 \n）、crlf（统一 \r\n）
newline = auto

# 进程/线程 ID 显示格式：decimal（十进制）、hex（十六进制）、none（不显示）
pid format = decimal
tid format = decimal

# 配置文件变更检查间隔（秒），0 表示禁用热加载
# Linux 优先 inotify，此值为 inotify 不可用时的回退轮询间隔
hot reload interval = 5

# 是否由库注册 SIGHUP 处理器触发配置热加载检查（默认关闭，避免与宿主应用信号处理冲突）
# 仅 HPULOGC_ENABLE_HOT_RELOAD=ON 时有效；handler 内仅设置原子标志，实际检查由库后台执行；
# 与 `hot reload interval = 0`（禁用轮询）独立：信号为显式触发，仍生效。
# 注册失败（如宿主已安装 SIGHUP handler）时输出警告并继续（热加载仅靠 inotify/轮询）。
signal reload = false


# ============================================================================
# [formats] 日志格式模板
# ============================================================================
# 模板值为 = 后整行，建议用双引号包裹。
# 可用占位符：%level %time %pid %tid %file %line %func %msg %category %n %%
# ============================================================================
[formats]

minimal     = "%level: %msg%n"
standard    = "%time [%level] %msg%n"
categorized = "%time [%level] [%category] %msg%n"
detailed    = "%time [%level] [pid:%pid tid:%tid] [%file:%line %func] [%category] %msg%n"
json        = "{\"time\":\"%time\",\"level\":\"%level\",\"category\":\"%category\",\"pid\":%pid,\"tid\":%tid,\"file\":\"%file\",\"line\":%line,\"msg\":\"%msg\"}%n"


# ============================================================================
# [outputs] 输出目标定义
# ============================================================================
# 行内参数为逗号分隔的 key=value，值可用双引号包裹（含空格、逗号、等号的值）。
# console: stream=stdout|stderr, color=true|false
# file: path, file perms, dir perms, rotate=none|size|time|both,
#       max size, time unit=hour|day|week|month, max files,
#       fsync, symlink latest, rotate naming
# socket: 预留，当前版本定义即启动失败（HPULOGC_ERR_CONFIG）
# ============================================================================
[outputs]

console_out  = console, stream=stdout, color=true
console_err  = console, stream=stderr, color=true

main_log     = file, path=/var/log/myapp/app.log, rotate=size, max size=100mb, max files=10, file perms=0644, dir perms=0755, symlink latest=true
error_log    = file, path=/var/log/myapp/error.log, rotate=time, time unit=day, max files=30, file perms=0644
debug_log    = file, path=/var/log/myapp/debug.log, rotate=both, max size=50mb, time unit=day, max files=7
audit_log    = file, path=/var/log/myapp/audit.log, rotate=none, fsync=true, file perms=0600

# Windows 示例：
# main_log_win = file, path="C:\Logs\MyApp\app.log", rotate=size, max size=100mb, max files=10


# ============================================================================
# [buffer] 环形缓冲区运行时参数（仅 init 时生效，热加载忽略变更）
# ============================================================================
# 注意：concurrency 与 lockfree 由编译期构建决定，不可在此设置
# ============================================================================
[buffer]

buffer size = 1mb
overflow policy = discard


# ============================================================================
# [async] 异步写入参数（HPULOGC_ENABLE_ASYNC=OFF 时此节被跳过）
# ============================================================================
# batch size        批量提交条数（1~65535）
# flush interval    未攒满 batch 时的强制提交间隔（毫秒，1~60000）
# shutdown timeout  shutdown 等待异步写入完成的超时（毫秒，0 = 无限等待）
# ============================================================================
[async]

batch size = 64
flush interval = 100
shutdown timeout = 5000


# ============================================================================
# [throttle] 限流与采样（HPULOGC_ENABLE_THROTTLE=OFF 时此节被跳过）
# ============================================================================
# 执行顺序：先限流，后采样（§4.9③）。
# global rate limit    全局令牌桶速率（条/秒），0 表示不限流；桶容量 = burst size
# per category rate limit  每个 category 独立令牌桶速率（条/秒），0 表示不限流；
#                      与全局限流独立检查，任一超限即丢弃
# sampling rate        采样率（0.0 ~ 1.0），确定性 1/N 采样：
#                      N = round(1/采样率)，全局共享原子计数器，每 N 条放行 1 条；
#                      0.0 = 全部丢弃，1.0 = 全部放行
# burst size           令牌桶容量（条），全局桶与分类桶共用
# ============================================================================
[throttle]

global rate limit = 0
per category rate limit = 0
sampling rate = 1.0
burst size = 100


# ============================================================================
# [rules] 日志路由规则
# ============================================================================
# 语法：category.level = format_name, output_name1, output_name2, ...
#
# selector 解析（规范性）：以最后一个 '.' 分割为 category 与 level；
# level 部分不得包含 '.'（否则报错 + 行号）。category 层级以 '.' 分隔。
#
# category 匹配：
#   *          匹配所有分类
#   name       精确匹配分类名
#   name.*     匹配 name 及其子分类（以 '.' 分隔的层级前缀，含 name 自身）
#
# level 匹配：
#   *          所有级别
#   LEVEL      精确匹配（如 TRACE）
#   A~B        范围匹配，含端点（如 ERROR~FATAL 表示 ERROR 及以上）；
#              min > max 为配置错误（§10.4）
#
# 匹配语义（规范性，zlog 风格，纯顺序）：
#   - 从上到下扫描，第一条 category 匹配且 level 落在范围内的规则生效
#   - 仅生效一条规则；该条日志按此规则的 outputs 各输出一份
#   - 兜底规则（category 为 *）必须放在具体规则之后，否则其后的规则永远不会命中
#   - 无任何命中规则时，按 [global] 的 default format + default outputs 兜底；
#     default outputs 为空时该日志被丢弃
#
# format/output 仅允许引用 [formats]/[outputs] 中已定义的名称，不支持内联定义
# ============================================================================
[rules]

app.debug.* = detailed, debug_log
security.* = json, audit_log, console_err
db.WARN~FATAL = standard, main_log
net.ERROR~FATAL = detailed, error_log, console_err
*.ERROR~FATAL = detailed, error_log
app.* = categorized, main_log
*.* = standard, console_out


# ============================================================================
# [advanced] 高级选项
# ============================================================================
[advanced]

# 日志注入防护：转义用户消息中的换行符和 ANSI 序列（不影响库自身的颜色序列；
# json 格式下不生效，见 §12）
escape injection = true

# 单条日志最大长度（字节，256~65536），超出后按 truncation marker 截断
# （同时约束生产者线程的消息体渲染上限，§9）
max log length = 4096
truncation marker = ...[TRUNCATED]

# fork() 后子进程行为：reinit / disable / inherit（含义见 §9）
fork behavior = reinit

# 是否允许 hpulogc_log_signal_safe() 实际输出（见 §7.3/§9）
signal safe = false

# fsync 策略：none / periodic / entry / shutdown（含义见 §9）
crash safety = shutdown

# 统计信息输出间隔（秒），0 表示禁用（min 构建下忽略）
stats interval = 0

# 统计输出目标：stderr 或 file（使用 stats file 路径）
stats output = stderr
# stats file = /var/log/myapp/stats.log
```

#### 10.3.1 rules 命中示例（以上方模板规则顺序为准）

| 日志（category, level） | 命中规则 | 输出 |
|--------------------------|----------|------|
| (app.debug.conn, INFO) | `app.debug.*` | debug_log |
| (app.http, INFO) | `app.*` | main_log |
| (app.http, ERROR) | `*.ERROR~FATAL` | error_log |
| (db.query, WARN) | `db.WARN~FATAL` | main_log |
| (db.query, ERROR) | `db.WARN~FATAL` | main_log（db 规则在前，优先命中） |
| (net.sock, ERROR) | `net.ERROR~FATAL` | error_log + console_err |
| (security.login, TRACE) | `security.*` | audit_log + console_err |
| (ui.render, INFO) | `*.*` | console_out |

### 10.4 配置解析器行为约束

| 场景 | 行为 |
|------|------|
| 配置文件不存在 | 返回 `HPULOGC_ERR_CONFIG`，不 fallback |
| 配置文件为非普通文件（目录/FIFO/设备等） | 启动失败，返回 `HPULOGC_ERR_CONFIG` |
| 语法错误（节头格式、缺 `=`、缺值、物理行超 1024 字节等） | 返回错误 + 行号，不 fallback |
| 节出现顺序违反 §10.2 规范顺序，或出现未知节名 | 启动失败，返回 `HPULOGC_ERR_CONFIG`，报行号 |
| 未知键 | `strict init = true`：启动失败（`HPULOGC_ERR_CONFIG`，报行号）；`false`：stderr 警告并忽略该键 |
| 引用未定义 format/output（含 default format、default outputs、stats file 引用） | 启动失败，报错 |
| 在 rules 中内联定义 format/output | 不支持；视同引用未定义名称，启动失败 |
| `[formats]`/`[outputs]` 中重复定义同名键 | 启动失败（`HPULOGC_ERR_CONFIG`），报重复行号 |
| `[rules]` 中的同名键 | 允许（即多条规则），按出现顺序生效 |
| `[global]`/`[buffer]`/`[async]`/`[throttle]`/`[advanced]` 中重复键 | 后值覆盖前值 + stderr 警告 |
| rule 的 min level > max level | 启动失败，返回 `HPULOGC_ERR_CONFIG` |
| 两个及以上 file output 的路径（规范化后）相同 | 启动失败，返回 `HPULOGC_ERR_CONFIG` |
| `rotate naming` 模板既不含 `{index}` 也不含 `{timestamp}` | 启动失败，返回 `HPULOGC_ERR_CONFIG` |
| outputs 数量 > `HPULOGC_MAX_OUTPUTS` 或 rules 条数 > `HPULOGC_MAX_RULES` | 启动失败，返回 `HPULOGC_ERR_CONFIG` |
| `buffer size < 2 × max log length` | **非错误**：自动提升 buffer size（下限 4KB）+ stderr 警告（§4.3） |
| 配置值超出合法范围 | 裁剪到最近有效值 + stderr 警告 |
| 配置值合法但当前构建不支持 | **启动失败**，返回 `HPULOGC_ERR_CONFIG`（如：无锁构建 `overflow policy = wait`、无锁 MPSC 构建 `overflow policy = overwrite`、定义 socket 类型 output） |
| 配置项属于已裁剪功能 | 静默跳过整个配置节（节内的单个键亦静默忽略） |
| 热加载时新配置非法 | 保留旧配置，错误详情输出至 stderr |
| `[build]` 节 | 可选：缺失合法；存在时其键值与实际构建信息比对，不一致 → 忽略写入值，输出警告 |

> `strict init` 仅影响"未知键"的处理级别（与 zlog 的软/硬失败分级对齐，但范围更收敛）；语法错误、缺值、节顺序错误、重复定义、引用未定义名称无论 `strict init` 取值均启动失败。

### 10.5 热加载行为明细（规范性）

| 配置项 | 热加载行为 |
|--------|-----------|
| `global.level`、`timezone`、`time format`、`newline`、`pid format`/`tid format` | 立即生效 |
| `global.default format` / `global.default outputs` | 立即生效（引用校验失败则整体回滚） |
| `[formats]` | 立即生效（整表原子替换） |
| `[rules]` | 立即生效（整表原子替换） |
| `[outputs]` | 原子替换：新增的 file output 打开文件（失败则整体回滚旧配置并报错）；被删除的 file output 先 flush 再关闭 fd；**路径与全部参数均未变更的 file output 复用已打开 fd，不关闭重开**；console 类型 output 的增删随原子替换直接生效（console 无持久 fd） |
| `[throttle]` | 立即生效 |
| `async.batch size` / `async.flush interval` | 立即生效 |
| `async.shutdown timeout` | 立即生效 |
| `advanced.escape injection` / `max log length` / `truncation marker` / `crash safety` | 立即生效 |
| `advanced.fork behavior` | 立即生效（仅影响其后的 fork） |
| `advanced.signal safe` | 立即生效（影响其后的 `hpulogc_log_signal_safe` 调用是否输出） |
| `advanced.stats interval` / `stats output` / `stats file` | 立即生效 |
| `[buffer]`（buffer size / overflow policy） | **忽略变更**（重建缓冲区代价大），输出一次提示日志 |
| `global.hot reload interval` | 立即生效（下一次检查按新间隔） |
| `global.capture source loc`、`timestamp source` | 立即生效 |
| `global.strict init` | 仅 init 时生效；热加载解析新配置时，未知键按当前生效的 `strict init` 值处理 |
| `global.signal reload` | 立即生效（true 时注册 SIGHUP handler，注册失败输出警告） |

> 热加载的替换必须是原子的：任一环节失败（如新 output 打开失败、引用未定义名称）则整体回滚到旧配置，保证不存在半新半旧状态。

---

## 11. 构建与分发

| 项目 | 要求 |
|------|------|
| 语言标准 | `HPULOGC_C_STANDARD`（99 / 11，默认 99）驱动 `CMAKE_C_STANDARD`，并联动原子后端自动探测（§4.3） |
| 原子后端 | `HPULOGC_ATOMIC_BACKEND = auto \| stdatomic \| gcc-atomic \| gcc-sync \| msvc-interlocked`（默认 auto；`gcc-atomic`/`gcc-sync` 仅 Linux/macOS，`msvc-interlocked` 仅 Windows，交叉指定为 CMake 配置错误） |
| 环形缓冲实现 | `HPULOGC_LOCKFREE`（OFF/ON）二选一编译 `ringbuf_locked.c` / `ringbuf_lockfree.c`（§3.3） |
| 警告级别 | GCC/Clang: `-Wall -Wextra`；MSVC: `/W4` |
| Sanitizer | `HPULOGC_SANITIZER = none \| address \| thread \| undefined`（默认 none；address 与 undefined 可叠加，thread 与 address 互斥——同时指定为 CMake 配置错误，替代原一键式选项） |
| Doxygen | 公共 API 全量 Doxygen 文档化；CI 以 `WARN_AS_ERROR = YES` 运行 doxygen 作为门禁 |
| 静态分析 | 集成 clang-tidy / cppcheck |
| 代码格式化 | `.clang-format` 统一风格 |
| 交叉编译 | 支持 CMake Toolchain File |
| 安装 | `make install` 安装头文件、库、`hpulogcConfig.cmake` |
| 包管理 | 提供 vcpkg / Conan recipe（P2） |

---

## 12. 日志内容规范

| 属性 | 说明 |
|------|------|
| 时间戳源 | `CLOCK_REALTIME`（默认）或 `CLOCK_MONOTONIC`（`%time` 固定输出相对 init 的"秒.微秒"，`time format` 忽略） |
| 时区 | 本地（默认）或 UTC |
| 编码 | 统一 UTF-8；Windows 下 `wchar_t` → UTF-8 |
| 源码位置 | `__FILE__`/`__LINE__`/`__func__`；编译期 `HPULOGC_ENABLE_SOURCE_LOC=OFF` 或运行时 `capture source loc = false` 时 `%file`/`%line`/`%func` 展开为空串 |
| 换行符 | `%n` 默认跟随平台：`\n`（Unix）或 `\r\n`（Windows）；可通过 `global.newline` 强制 `lf` / `crlf` / `auto` |
| JSON 转义 | `json` 格式对 `%msg`/`%file`/`%category`/`%func` 的展开结果自动做 JSON 字符串转义（`"` `\` 与控制字符），**仅执行 JSON 转义、跳过 `escape injection` 注入转义**（避免双重转义），且不受该开关影响；`json` 输出中 pid/tid **强制十进制**（忽略 `pid format`/`tid format` 的 hex/none 设置，保证 JSON 合法） |

---

## 13. 测试要求

### 13.1 单元测试

- 覆盖：环形缓冲区（有锁/无锁两套实现分别覆盖）、原子后端（每个后端的行为等价性测试）、格式化器、配置解析器、文件写入器、轮转器。
- 框架：CTest 集成或自实现 ASSERT 宏。
- 覆盖率：模块行覆盖率 ≥ 90%（平台相关 glue 代码与 `#ifdef` 独占分支可豁免，豁免范围在覆盖率报告中显式列出）。

### 13.2 集成测试

- 覆盖所有功能组合和边界场景：
  - 缓冲区满/空、配置错误/缺失、磁盘满/只读
  - 溢出策略（discard/overwrite/wait）行为与 `dropped`/`overwritten`/`throttled` 计数正确性
  - 高频并发、fork() 子进程（reinit 惰性重建/disable/inherit）、热加载非法配置、热加载 output 增删、SIGHUP 触发热加载、json 格式输出合法性校验

### 13.3 性能测试

- 基准测试：延迟（ns）、吞吐量（logs/sec）。
- 压力测试：≥ 24h 高并发，验证无泄漏/无竞争/无死锁（**发布前执行**：夜间/手动触发，不进常规 CI）。
- 对比报告：hpulogc vs printf vs zlog vs spdlog（C++ 参照）。

### 13.4 模糊测试

- INI 解析器 Fuzz 测试（AFL++ / libFuzzer），CI 中运行。

### 13.5 自动化

- CMake + CTest 一键运行。
- CI 矩阵（各平台维度对齐）：
  - Ubuntu（GCC/Clang）× {x86_64, ARM64} × {C99, C11} × {有锁, 无锁} × {SPSC, MPSC}
  - macOS（Apple Clang）× {x86_64, ARM64} × {C99, C11} × {有锁, 无锁} × {SPSC, MPSC}（Phase 3）
  - Windows：MSVC × x64 × C99（C11 任务于 VS2019 16.8+ 镜像上启用）× {有锁, 无锁} × {SPSC, MPSC}；MinGW-w64 × x64 × {C99, C11} × {有锁, 无锁} × {SPSC, MPSC}
  - Sanitizer：Linux 每次 CI 运行 ASan + UBSan（代表性构建组合）；TSan 夜间任务
- CI 按平台分阶段启用（§16）；Phase 2/3 验收含"公共代码零修改"检查（§3.3）。
- Valgrind / Dr. Memory 零泄漏验证。
- gcov / lcov 覆盖率报告 + 阈值门禁。

---

## 14. 文档与示例

| 文档 | 内容 |
|------|------|
| `README.md` | 简介、快速开始、构建说明 |
| `API.md` | 完整 API 参考（Doxygen） |
| `BUILD.md` | 各平台编译指南 |
| `PORTING.md` | 新平台移植 checklist（以 §3.3 平台接口契约为准） |
| `examples/` | 最小示例、异步示例、多 Category 示例、自定义格式示例 |

---

## 15. 工程约束汇总

| 类别 | 约束 |
|------|------|
| ABI 稳定性 | 同 Major 版本内 ABI 兼容；`hpulogc_config_t`、`hpulogc_build_info_t`、`hpulogc_stats_t` 等公开 POD 结构**只允许尾部追加字段**；运行时内部句柄/实现对象使用 opaque pointer |
| 向后兼容 | 配置文件格式同 Major 版本内向后兼容 |
| 编码与命名 | 源文件 UTF-8 无 BOM（带 `/* -*- coding: utf-8 -*- */` 声明头，当含非 ASCII 时）；注释仅英文；标识符 snake_case；公共 API 全量 Doxygen（与 AGENTS.md 一致，CI 门禁见 §11） |
| Git 提交 | 建议 Conventional Commits |
| 版本管理 | 语义化版本 2.0 |
| 代码风格 | clang-format 统一 |

---

## 16. 实施阶段计划（规范性）

实现严格按以下三个阶段推进，阶段顺序固定，不得跳过或并行（文档、CI 脚本等辅助工作除外）。所有平台差异必须收敛到 §3.3 的平台层与原子后端；后续阶段对公共代码（`include/` 与 `src/` 非平台目录）**零修改**是各阶段验收条件。

### 16.1 Phase 1 — Linux（全功能）

范围：在 Linux 上完成本规范的全部功能实现，并为 Windows/macOS 预留全部扩展接口。

1. **冻结平台接口契约**：定义并评审 §3.3 所列平台接口头（同步原语、线程、时间、文件/目录、路径、watcher、tid、导出宏），作为 win32/darwin 的移植依据；契约一经冻结，Phase 2/3 不得要求修改（如需修改视为 Phase 1 缺陷）。
2. **实现 POSIX/Linux 平台层**：`src/platform/posix/`（与 macOS 共享）与 `src/platform/linux/`（inotify watcher 等）全部实现。
3. **原子层**：实现 `stdatomic`、`gcc-atomic`、`gcc-sync` 三个后端及行为等价性单元测试；CI 以 `-std=c99` 与 `-std=c11` 双标准分别构建，验证后端自动选择正确。
4. **环形缓冲**：有锁与无锁两套实现全部完成；Linux 上验证 SPSC/MPSC × 有锁/无锁 全矩阵（含各溢出策略可用性约束，§4.3）。
5. **CMake 骨架**：平台检测（`WIN32`/`APPLE`/其余）、`HPULOGC_LOCKFREE` 选择 ringbuf 源文件（§3.3）、`HPULOGC_ATOMIC_BACKEND`、`HPULOGC_C_STANDARD`、`HPULOGC_ENABLE_*` 裁剪、四版本预设（§5）。
6. **预留**：`src/platform/win32/`、`src/platform/darwin/` 仅含接口头文件与占位说明，不参与编译。

**完成标准（DoD）**：§13 全部测试在 Linux 矩阵（GCC/Clang × x86_64/ARM64 × C99/C11 × 有锁/无锁 × SPSC/MPSC）通过；ASan/TSan/UBSan 零报告；§8 性能目标与 §5 min 体积验证达标。

### 16.2 Phase 2 — Windows

1. **实现 `src/platform/win32/`**：`SRWLOCK`/`CONDITION_VARIABLE`（有锁原语契约实现）、高精度时钟、`\` 路径与 UTF-8↔UTF-16 转换、文件 API 差异（`_commit` 对应 fsync；轮转清理用 `FindFirstFile` 系实现目录扫描语义）、无 inotify → 按 `hot reload interval` 轮询 mtime/大小。
2. **实现 `src/atomic/atomic_msvc.h`**：C99+ 全部语言标准统一使用 MSVC `Interlocked*`（MinGW-w64 亦通过 `intrin.h` 使用同一族，保证 Windows 后端唯一，见 §4.3）。
3. **MSVC 适配**：`/W4` 零警告、`##__VA_ARGS__` 兼容策略落地（§7.3）、`file perms`/`dir perms`/`symlink latest` 忽略并输出警告（§4.7）、默认换行 `\r\n`（§12）、控制台启用 VT 处理（`ENABLE_VIRTUAL_TERMINAL_PROCESSING`）以支持 ANSI 彩色（§4.7）。
4. **完成 Windows CI**（MSVC/MinGW × x64）。

**DoD**：Windows 矩阵测试全绿；内存检查（MSVC ASan / Dr. Memory）零泄漏；相对 Phase 1，公共代码零修改（diff 验收）。

### 16.3 Phase 3 — macOS

1. **实现/验证 `src/platform/darwin/`**：复用 POSIX 共享层，补充 darwin 专属（`pthread_threadid_np`、kqueue watcher 或轮询回退、时钟兼容）。
2. **Apple Clang 双标准验证**：C11 `<stdatomic.h>` 与 C99 `__atomic_*` 两条原子路径均通过测试（禁止 `OSAtomic*`，§4.3）。
3. **双架构**：x86_64 + ARM64（Apple Silicon），universal binary 可选。
4. **完成 macOS CI**。

**DoD**：macOS 矩阵测试全绿、sanitizer 零报告；相对 Phase 1/2，公共代码零修改。

### 16.4 阶段与优先级映射

P0/P1 功能项在 Phase 1（Linux）完成；Windows/macOS 专属适配分别随 Phase 2/3 交付；P2 增强项中，彩色输出的 Linux 部分随 Phase 1 交付、Windows 控制台（VT）适配随 Phase 2 交付，其余 P2 项在 Phase 1 完成接口预留，其平台差异行为随所在阶段交付。映射总览见 §17。

---

## 17. 需求优先级总览

> 优先级仅表示实现排期顺序，不改变本规范中已定义的行为与默认值（如彩色输出的 Linux 部分随 Phase 1 交付、Windows VT 适配随 Phase 2 交付，已实现部分受 `HPULOGC_ENABLE_COLOR` 裁剪控制，选项默认 ON）。实现阶段划分见 §16：P0/P1 项于 Phase 1（Linux）交付；P2 项的平台差异随 Phase 2/3 交付。

| 优先级 | 项目 |
|--------|------|
| **P0（必须）** | 纯 C 实现、跨平台、线程安全、6 级日志、环形缓冲（有锁）、文件+控制台输出、INI 配置、单元测试、CMake 构建、API 签名、导出符号、错误处理、编译警告、Sanitizer、安装规则、示例、配置解析器行为约束 |
| **P1（重要）** | 无锁队列、MPSC 模式、批量提交、日志轮转、溢出策略、集成测试、性能测试、性能量化目标、信号处理/fork 安全、配置热加载、多 Category、时间戳/时区、模糊测试、ABI 稳定性、C99/C11 双标准原子实现、**编译期裁剪体系（`HPULOGC_ENABLE_*` 等）、四版本构建预设** |
| **P2（增强）** | 彩色输出（Linux 部分随 Phase 1，Windows VT 适配随 Phase 2）、格式定制、包管理器、静态分析、文档生成、移植指南、`hpulogc_strerror` 错误描述 API |

---

## 附录 A：zlog 实现对比与待决策项

> **调研基线**：HardySimpson/zlog master 分支（截至 2026-09-18 提交，`src/version.h` 标识 1.2.18；最新 release 亦为 1.2.18，2024-07-03 发布，修复 CVE-2024-22857）。master 含未发布改动：可选后台消费者线程（`use_writer_thread`/`fifo_size`）、rwlock 写者饥饿让路修复、`zlog_init_from_string` 等。
>
> **性质**：本附录为决策辅助材料，**不构成规范性要求**（正文已明确写入的 zlog 词法采纳项除外，见 §10.1）。
>
> **使用方式**：A.1 各项由文档维护者逐项决策——采纳项转入正文规范性条款，不采纳项移入 A.2，状态列同步更新。

### A.1 待决策项（zlog 做法可能更优）

| # | 主题 | zlog（master）做法 | 本文档现行方案 | 对比与建议 | 状态 |
|---|------|--------------------|----------------|------------|------|
| 1 | 时间戳按秒缓存 | 每个时间占位符独立缓存槽，strftime 结果在同一秒内复用（每秒每占位符至多一次 `localtime_r`+`strftime`），热路径时间格式化近乎零成本 | 未规定（`%time` 每条重新渲染） | **建议采纳**为实现要求；代价仅每占位符一个缓存槽 + 每秒一次刷新，对 §8 延迟目标收益明显 | 待决策 |
| 2 | per-category 预计算路由 + 级别位图 | category 登记时预计算命中规则列表与级别位图；每条日志先 O(1) 位测试短路，再遍历命中规则；热加载用双缓冲 update/commit/rollback | §4.9 步骤②⑥ 仅定义语义，未规定实现结构 | **建议采纳**为实现要求；与本文 category 登记表、热加载原子替换天然契合 | 待决策 |
| 3 | level_enabled 检查 API | `zlog_level_enabled(cat, level)` + `zlog_fatal_enabled(cat)` 等宏 + printf format 属性（编译期检查格式串） | 无对应 API | **建议采纳**（P1）：`hpulogc_level_enabled(category, level)`、`HPULOGC_xxx_ENABLED(cat)` 宏、便捷宏加 format 属性；避免调用方为被过滤日志做昂贵的参数构造 | 待决策 |
| 4 | 配置校验 CLI | 附带 `zlog-chk-conf` 独立校验工具（CI 可用） | 无 | **建议采纳**（P2）：`hpulogc_chk_conf`，退出码报告错误行号 | 待决策 |
| 5 | 外部轮转检测 | 静态文件输出每条 `stat` 比对 inode/dev，检测外部 logrotate 换文件后重开（WatchedFileHandler 语义） | 仅"运行期写失败时重开一次"（§9） | zlog 每条 `stat` 有可测开销（与 §4.6 O(1) 检查要求冲突）；建议**节流采纳**（每 N 条 + 写失败时比对 inode）或保持现状 | 待决策 |
| 6 | `!LEVEL` 取反匹配 | 规则级别支持裸 `LEVEL`（≥ 语义）、`=LEVEL`（精确）、`!LEVEL`（除该级别外）、`*` | `*` / `LEVEL`（精确）/ `A~B`（范围） | `!LEVEL` 语法成本低、表达力补充，**建议采纳**；注意 zlog 裸 LEVEL 为 ≥ 语义，与本文"精确"不同，若采纳需保持本文语义并标注 | 待决策 |
| 7 | 格式占位符扩展 | `%d`/`%g`（本地/UTC 时间）、`%ms`/`%us`、`%k`（系统级 tid）、`%H`（主机名）、`%F`/`%f`（全/短文件名）、printf 宽度/精度修饰符（`%-20.30c`） | 9 个占位符 + `%f` 微秒扩展（§10.3/§12） | **建议至少采纳** `%g`（UTC 时间）与宽度/精度修饰符；`%k`/`%H`/短文件名低优先；本文 `%f` 与 zlog `%ms`/`%us` 语义重叠，若引入别名须在 §12 定义共存规则 | 待决策 |
| 8 | 用户自定义级别 | `[levels]` 节自定义级别（`NAME = int[, syslog_level]`），内置级别含 NOTICE | 固定 7 级枚举（§4.1/§7.6，ABI 冻结） | **建议不引入**（级别集稳定性优先、API 简单）；如确需再评估运行时注册制 | 待决策 |
| 9 | MDC | 每线程 MDC（put/get/remove + `%M(key)` 占位符） | 无 | **建议不引入**（异步模式下 MDC 属生产者线程上下文，跨线程传递语义复杂；请求级上下文建议调用方自行并入 `%msg`） | 待决策 |
| 10 | record / syslog / pipe 输出 | `$record` 回调、`>syslog[,facility]`（级别映射）、`\|pipe`（popen） | 仅 console/file；socket 骨架预留（定义即启动失败） | record 回调**建议列 P2**（纯 C 函数指针、零依赖、可对接收集系统）；syslog/pipe **建议不引入**（零依赖定位、Windows 无 syslog、popen 子进程管理复杂） | 待决策 |
| 11 | 轮转归档命名 | `#r` 滚动重编号（logrotate 风格级联）/ `#s` 序号递增，支持零填充宽度（`#2s`），归档路径可含时间占位符 | `{base}`/`{timestamp}`/`{index}` 模板 + `max files` 清理 + `.latest` 软链（§4.6） | 本文模板更灵活可控；`rotate naming` 仅用 `{index}`（不含 `{timestamp}`）即等效 `#s` 序号风格，§4.6 已说明该等效性，无需新增机制 | 待决策 |
| 12 | 双映射环形缓冲 | 异步消费者用 `memfd_create` + 两次 `MAP_FIXED` 双映射环形页，跨边界记录单次连续 memcpy；per-record RESERVED/COMMITTED 原子标志（Linux 专属，zlog 因 memfd 未支持 Windows） | 未规定无锁实现细节 | 可作为 Phase 1 Linux 无锁 SPSC 的**可选优化**；必须保留 macOS/Windows 通用回退（尾部分段拷贝）；因跨平台实现分叉，列为可选优化而非规范 | 待决策 |
| 13 | shutdown/热加载线程协同 | flush/退出经队列内命令记录 + 消费者完成握手；生产者 per-thread 状态引用计数延迟释放（消费者处理完最后一条后才释放）；注：其退出路径存在忙等缺陷，近期多个修复围绕这些缝隙 | §7.5/§10.5 定义了行为，未规定机制 | **建议采纳**为实现要求（命令记录 + 条件变量握手 + 引用计数延迟释放；用条件变量而非 zlog 式忙等） | 待决策 |
| 14 | per-thread 缓冲增长策略 | 每线程缓冲 1KB 起步、按增量增长（全局可配 `buffer min`/`buffer max`，默认上限 2MB），不缩减，稳态零 malloc；`buffer max=0` 为无上限 | `max log length`（默认 4KB）硬截断（§9）；v0.2 已定义生产者渲染上限 = `max log length` | **建议组合**：预分配 + 按需增长至 `max log length` 上限即截断（与 §9 渲染上限条款方向一致，兼得稳态零 malloc 与硬上限；避免 zlog 无上限模式的内存风险）；采纳与否仅影响 per-thread 缓冲增长策略实现 | 待决策 |

### A.2 明确不采纳项（本文方案更优，记录结论）

| # | 主题 | zlog 做法 | 本文档方案（保持） | 不采纳理由 |
|---|------|-----------|--------------------|------------|
| 1 | 全局 rwlock 热路径 | 每条日志持有全局 env 读锁（含格式化与 `write()`）；为修写者饥饿引入让路协议（作者自测损耗 ~1.5% 吞吐，仍有 `sched_yield` 自旋） | 生产者热路径无全局锁（环形缓冲 + 消费者线程，§4.2/§4.4） | zlog 同步模型的扩展性天花板；本文架构天然规避该锁 |
| 2 | 值校验宽松 | 数值不可解析/越界静默变 0；布尔"非 false 即 true"（拼写错误静默生效）；重复键静默 last-wins | 超范围 clamp + 警告；布尔严格校验；重复键检测（`[formats]`/`[outputs]` 报错、其余警告覆盖，§10.4） | 静默错误配置违背 fail-fast 原则（§10.1） |
| 3 | reload 全量重建 | reload 重建全部 rule 并关闭重开所有 fd（未变更文件也重开，存在窗口） | 未变更 output 复用 fd，仅增删/参数变更项重建（§10.5） | 减少重开窗口与开销；语义已定义 |
| 4 | POSIX 跨进程"锁文件" | rotater 以 `open`/`close` 充当跨进程锁（POSIX 上未实际发出任何锁系统调用，仅 Windows `CreateFile` 独占有效） | 不承诺多进程安全（§9） | 定位单进程多线程；zlog 该机制在 POSIX 上实为无效保护，不值得效仿 |
| 5 | 通用键 value 截断 | 通用键 value 以 `sscanf %s` 截断于首个空白（仅 `default format` 特例取整行） | value 统一取 `=` 后整行（引号可包裹，§10.1） | 本文为严格超集：允许含空格的值（`time format`、`truncation marker`、路径）；zlog 风格配置在本文规则下解析结果一致 |

---

## 修订记录

### v0.2（2026-09-19）

基于 v0.1 全文评审（30 项）修订。**矛盾修复**：

1. per-category 级别语义统一为"覆盖"（§4.1/§4.9②/§7.3）。
2. `default format`/`default outputs` 兜底语义补全：未命中规则时按兜底 format + outputs 输出，为空则丢弃（§4.9⑥/§7.2/§7.6/§10.2/§10.3/§10.4/§10.5）。
3. §16/§17 优先级映射对齐：编译期裁剪体系与四版本预设上调 P1（§16.4/§17）。
4. 编译器矩阵与 C11 要求对齐：GCC ≥ 4.9；MSVC C11 构建需 2019 16.8+（§2.1）。
5. 定义生产者渲染上限 = `max log length`，init 校验 `buffer size ≥ 2 × max log length`，消除 wait 策略死锁边界（§4.3/§4.9④/§9/§10.4）。
6. 缓冲区"仅 init 时指定、热加载忽略"表述修正（§4.3/§10.5）。

**行为补充**：

7. `HPULOGC_ENABLE_CATEGORY=OFF` 时规则按 `*` 匹配、`%category` 展开空串（§4.8）。
8. 限流/采样语义定死：先限流后采样、全局/分类独立令牌桶、确定性 1/N 采样、stats 新增 `throttled` 字段（§4.9③/§7.3/§10.3）。
9. SIGHUP 触发增加 `signal reload` 开关与 handler 所有权定义（§4.5/§10.3/§10.5）。
10. category 为 NULL/空串等价 `"*"`；不做字符集校验（§4.9）。
11. per-output `fsync` 与全局 `crash_safety` 取更严格者；严格度 none < shutdown < periodic < entry（§7.6/§9）。
12. 热加载表补全 default format/outputs、`[advanced]` 全部键、shutdown timeout、console 增删、strict init（§10.5）。
13. `hpulogc_log_signal_safe` 始终 async-signal-safe；`signal_safe=false` 时静默丢弃（§7.3/§9）。
14. `fork behavior=reinit` 惰性重建语义（child handler 仅置脏标记）（§9）。
15. 配置节均可省略、`[build]` 可选；全静默误配提示（§10.2/§10.4）。
16. 新增 `hpulogc_config_default()` 强制初始化约定；`hpulogc_init(NULL)` = `init_default()`；代码内配置非法值 fail-fast（不 clamp）；声明代码内配置为配置文件子集并列出专属键（§7.2/§7.3/§7.6）。
17. 校验补全：rule min>max、file output 重复路径、stats file 引用、`written` 按条计数、outputs/rules 数量上限（§7.3/§10.4）。
18. json 格式仅 JSON 转义（跳过注入转义）+ pid/tid 强制十进制（§12）。

**改进与一致性**：

19. 级别枚举更名 `HPULOGC_LEVEL_*`，消除与便捷宏 `HPULOGC_xxx` 的同名隐患（§4.1/§7.3/§7.6）。
20. min 预设 `INI=OFF`（定位嵌入式、保护 8KB 体积预算；需要配置文件者自行组合选项）（§5）。
21. `##__VA_ARGS__` 可移植性方案改写为可执行的检测序列（§7.3）。
22. CI 矩阵补齐 SPSC/MPSC、C99/C11 维度，sanitizer 任务显式化（§13.5）。
23. `HPULOGC_SANITIZER`（address/thread/undefined）替代一键式 `HPULOGC_ENABLE_SANITIZER`（§11）。
24. 许可证定为 MIT（§1）。
25. 新增 errno 保持要求（§9）。
26. 诊断信息统一 stderr；`hpulogc_strerror` 列入 P2（§7.4/§17）。
27. §4.8 "HPULOGC_ENABLE_*" 措辞修正（§4.8）。
28. AGENTS.md 工程约束纳入（Doxygen 门禁、UTF-8 无 BOM、注释英文、snake_case）（§11/§15）。
29. 配置词法补充：引号内 `,`/`=` 不作分隔；outputs/rules 数量上限（§10.1/§10.4）。
30. 无锁 MPSC 构建下 `overwrite` 不可用（fail-fast，与 wait 同类）（§4.3/§5/§10.4）。

---

*（全文完）*
