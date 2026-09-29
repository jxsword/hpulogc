# hpulogc — High Performance Log in C

[![CI](https://github.com/jxsword/hpulogc/actions/workflows/ci.yml/badge.svg)](https://github.com/jxsword/hpulogc/actions/workflows/ci.yml)
[![CI (Windows MSVC)](https://github.com/jxsword/hpulogc/actions/workflows/win_msvc.yml/badge.svg)](https://github.com/jxsword/hpulogc/actions/workflows/win_msvc.yml)

纯 C99/C11、零第三方依赖、跨平台的高性能日志库（Linux / Windows / macOS）。

- **无锁环形缓冲**（SPSC/MPSC/MPMC，discard/overwrite 溢出策略）与有锁实现二选一；MPMC 支持 N 个消费者线程（rd_v0.3）
- **异步消费者**线程 + 批量同步落盘（实测 ~5.2M logs/sec @64B，Windows 原生）
- **INI 配置**（zlog 兼容词法）+ 热加载（inotify/kqueue/SIGHUP/轮询）+ 原子替换回滚
- **多 Sink 体系**（rd_v0.6）：sink vtable ABI + 进程级注册、6 类内置
  sink（console/rollingfile/syslog/tcp/udp/null，其中网络型 tcp/udp 为
  v0.6.2 新增、POSIX 专属）、per-sink 异步队列 + 专属
  worker 批量写出、结构化字段（typed key=value）与 `%v` 占位符、
  per-sink 统计
- **路由规则**（类目选择器 × 级别范围）、5 个内置格式 + 自定义命名格式
- **轮转**（size/time/both、{base}/{timestamp}/{index} 模板、max files、.latest）
- **安全**：async-signal-safe 受限通道、fork 惰性重建、errno 保持、注入转义
- 编译期裁剪体系（`HPULOGC_ENABLE_*`）、原子后端可插拔、四版本预设

## 快速开始

```c
#include <hpulogc.h>

int main(void)
{
    hpulogc_init_default();                 /* level=INFO, stderr, standard 格式 */
    HPULOGC_INFO("app", "hello %s", "world");
    HPULOGC_ERROR("app", "error %d", 42);
    hpulogc_shutdown();
    return 0;
}
```

编译运行：

```bash
cmake -S . -B build -G Ninja
ninja -C build minimal && ./build/examples/minimal
```

自定义配置（代码内）：

```c
hpulogc_config_t cfg;
hpulogc_config_default(&cfg);
hpulogc_output_t out = { .type = HPULOGC_OUT_FILE, .path = "app.log" };
cfg.outputs = &out;
cfg.output_count = 1;
cfg.level = HPULOGC_LEVEL_DEBUG;
hpulogc_init(&cfg);
```

或使用 INI 配置文件（完整模板见 `docs/rd_v0.6.md` §10）：

```c
hpulogc_init_from_file("hpulogc.conf");
```

## 构建

依赖：CMake ≥ 3.16、C99/C11 编译器（GCC ≥ 4.9 / Clang ≥ 3.5 / MSVC ≥ 2019 16.8）。

### Linux / macOS

```bash
cmake -S . -B build -G Ninja                 # 默认 full 预设
ninja -C build
(cd build && ctest)                          # 全部测试
```

### Windows（MSVC，Phase 2）

要求 VS2019 16.8+（本仓库验证于 VS2026 / cl 19.50）+ Windows 10 SDK。
在 **x64 Native Tools Command Prompt**（或任何已 `vcvars64` 的环境）中：

```bat
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=cl
cmake --build build
ctest --test-dir build
```

或使用仓库自带包装脚本（自动加载 VS 环境）：

```bat
scripts\msvc_build.bat cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=cl
scripts\msvc_build.bat cmake --build build
scripts\msvc_build.bat ctest --test-dir build
```

MSVC 构建默认启用 `/W4 /utf-8`（零告警门禁）；`HPULOGC_SANITIZER=address`
映射到 `/fsanitize=address`（VS2019 16.9+）。换行默认跟随平台
（`newline = auto` → Windows 输出 `\r\n`）；`file perms`/`dir perms`/
`symlink latest` 在 Windows 上忽略并输出一次性告警（§4.7）。

### Windows（MinGW-w64，可选）

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build-mingw && (cd build-mingw && ctest)
```

MinGW-w64 与 MSVC 共用同一原子后端（`Interlocked*`，Windows 后端唯一）。

### macOS（Phase 3）

要求 macOS 10.12+（clock_gettime）；Apple Clang 即装即用：

```bash
cmake -S . -B build -G Ninja
cmake --build build && ctest --test-dir build
```

- 原子后端：C11 走 `<stdatomic.h>`、C99 走 `__atomic_*`（自动探测）；
- 同步原语复用 pthread；timedwait 用 Apple 的 relative 等待
  （macOS 的 `pthread_condattr_setclock` 仅支持 REALTIME）；
- 线程 ID 为 `pthread_threadid_np` 系统级 ID；
- 配置文件热加载为 kqueue（EVFILT_VNODE）后端，不可用时透明回退
  轮询（§16.3）；
- 双架构（x86_64/ARM64）与 ASan/TSan 由 GitHub Actions 矩阵覆盖。

常用选项（全平台一致）：

```bash
-DHPULOGC_BUILD_PRESET=min          # 嵌入式精简版（无配置文件/轮转/颜色）
-DHPULOGC_BUILD_PRESET=async_single # 无锁 SPSC 高吞吐版
-DHPULOGC_LOCKFREE=ON               # 无锁环形缓冲
-DHPULOGC_CONCURRENCY=SPSC          # 单生产者单消费者
-DHPULOGC_CONCURRENCY=MPSC          # 多生产者单消费者（默认）
-DHPULOGC_CONCURRENCY=MPMC          # 多生产者多消费者（Phase 4，见下节）
-DHPULOGC_C_STANDARD=11             # C11（默认 99）
-DHPULOGC_ATOMIC_BACKEND=gcc-atomic # 强制原子后端
-DHPULOGC_SANITIZER=address         # address / thread / undefined / address,undefined
-DHPULOGC_SINKS=console,rollingfile,syslog,tcp,udp,null  # 内置 sink 白名单裁剪（tcp/udp/syslog 为 POSIX 专属）
```

> 并发模式与是否无锁均为**编译期选择**，不可运行时更改；取值非法时
> CMake 配置期直接报错。

### MPMC 并发模式（Phase 4）

多个生产者线程 + N 个消费者线程共同消费同一环形缓冲，每条日志恰好被
一个消费者处理一次（不丢失、不重复，受溢出策略约束）：

```bash
cmake -S . -B build -G Ninja -DHPULOGC_CONCURRENCY=MPMC
```

代码内配置消费者数量：

```c
hpulogc_config_t cfg;
hpulogc_config_default(&cfg);
cfg.consumer_threads = 4;   /* 1~16，默认 1；仅 init 生效，热加载忽略 */
hpulogc_init(&cfg);
```

或 INI 配置文件：

```ini
[async]
consumer threads = 4
```

约束（完整规范见 `docs/rd_v0.3.md`）：

- 非法值：代码内配置 `0` 或 `> 16` 时 init 返回
  `HPULOGC_ERR_INVALID_ARG`；INI 值越界按惯例钳制到 1~16 并输出警告；
  非 MPMC 构建配置 `> 1` 时 init 返回 `HPULOGC_ERR_CONFIG`（值合法但
  构建不支持）；默认 1 保证配置文件跨构建可移植；
- 溢出策略可用性：有锁构建（SPSC/MPSC/MPMC）三种策略全支持；无锁
  MPMC 仅 `discard`（`overwrite`/`wait` 启动失败）；
- **顺序保证**：MPMC 不保证记录间全局输出顺序（每条记录自身原子
  完整）；依赖全局顺序的场景请使用 SPSC/MPSC 构建；
- 统计恒等式与单消费者构建一致；`hpulogc_build_info_t.concurrency`
  返回 `"mpmc"`，`sinks` 返回编译期内置 sink 类型列表（append-only
  ABI，rd_v0.6 新增）。

全矩阵验证：

```bash
# Linux / macOS（GCC/Clang × C99/C11 × 有锁/无锁 × SPSC/MPSC/MPMC + 4 预设）
bash scripts/run_matrix.sh

# Windows MSVC（C99/C11 × 有锁/无锁 × SPSC/MPSC/MPMC + 4 预设，含零告警门禁）
powershell -ExecutionPolicy Bypass -File scripts\run_matrix.ps1
```

### 多 Sink 体系（Phase 5）

每个输出实例都是一个 **sink**（vtable 抽象，`hpulogc_sink_ops_t`），
通过 `[outputs]`（INI）或 `hpulogc_config_t.sinks[]`（代码内，能力对等）
声明实例；内置 6 类：`console`、`rollingfile`（`file` 为别名）、
`syslog`（POSIX 专属，进程内单实例）、**`tcp` / `udp`**（网络型，v0.6.2
新增，POSIX 专属）、`null`。未注册的类型一律 fail-fast。每个实例可选
`async = on` 走 **per-sink 异步投递**：专属有界字节环队列 + 专属
worker 批量写出，队列满丢弃当前事件并计入该实例的 `dropped`（恒不阻塞
投递方；`queue size` 支持 64KB~16MB）。

```ini
# 节顺序有规范约束：[formats] 先于 [outputs] 先于 [rules]（§10.2）
[formats]
rendered = "%time [%level] %msg %v%n"   # %v 渲染结构化字段 k=v 列表

[outputs]
console0 = console, stream=stdout
applog   = file, path=logs/app.log, rotate=size, max size=10M, max files=7
worker   = file, path=logs/worker.log, async=on, queue size=1M
drop     = null

[rules]
*.*      = standard, console0, applog
worker.* = rendered, worker
```

**网络型 sink**（`tcp` / `udp`，rd_v0.6 §4.10.8；v1 POSIX 专属，Windows
构建不可注册；**推荐 `async = on`**，网络延迟与断线重试只影响该实例的
专属 worker）：

- `tcp`：行帧输出——每条记录渲染行追加 `\n`（兼容 syslog/rsyslog 行
  协议接收方）。`host`/`port` 必填；start 首连失败即 init 失败
  （fail-fast，配置错误早暴露）；运行期断线自动**指数退避重连**：
  `reconnect backoff`（默认 1 秒，1-60）起 ×2 至 `reconnect backoff max`
  （默认 30 秒）封顶，重连成功复位；退避窗口内事件计 `failed`，重连由
  事件驱动，不阻塞投递方。
- `udp`：无连接，每条记录恰好一个数据报；行长超过 `mtu`（默认 1472 =
  以太网 1500 − IPv4 20 − UDP 8，范围 576-65507）时**截断至 mtu 后仍
  投递**并计 `failed`（不做 IP 分片）；非阻塞 socket，`EAGAIN` 丢弃计
  `failed`；不保证送达（UDP + per-sink 异步恒 discard 双重明示）。
- 共同：投递语义 **at-most-once**（失败/退避窗口内事件不缓存重发）；
  热重载变更任何私有键即断连重建；记账走 per-sink
  written/dropped/failed（§4.10.3 恒等式闭合）。

```ini
[outputs]
net_tcp   = tcp, host=127.0.0.1, port=514, async=on, queue size=1M, reconnect backoff=1, reconnect backoff max=30
net_udp   = udp, host=127.0.0.1, port=514, async=on, queue size=1M, mtu=1472
```

**结构化字段**（`HPULOGC_*_EX` 宏族，I64/U64/F64/BOOL/STR 五类值，
键 64B / 值 256B / 每条 16 字段预算，超限计入全局 `fields_dropped`）：

```c
hpulogc_field_t f[2] = {
    { .key = "req_id", .value = { .type = HPULOGC_FIELD_U64, .v.u64 = 1001 } },
    { .key = "peer",   .value = { .type = HPULOGC_FIELD_STR,
                                  .v.str = { .s = "10.0.0.7", .len = 8 } } }
};
HPULOGC_INFO_EX("net", f, 2, "request done in %d ms", 42);
```

文本格式需在模板中放 `%v`（渲染为 `k=v k2=v2` 空格分隔、无字段为
空串）；内置 `json` 格式自动在对象末尾拼接 `,"fields":{...}` 成员。

**自定义 sink**（init 前注册，init 后注册表冻结；回调内禁止调用
`hpulogc_sink_*` 以外的库 API，稳态零 malloc）：

```c
static int my_configure(hpulogc_sink_t* s, const char* k, const char* v)
{
    return 0; /* 逐项接收私有键；未知键返回非 0（strict 时 fail-fast） */
}

static void my_emit(hpulogc_sink_t* s, const hpulogc_event_t* ev)
{
    fwrite(ev->line, 1, ev->line_len, stdout);   /* 行视图；ev->fields 为结构化视图 */
}

static const hpulogc_sink_ops_t g_my_ops = {
    "myout", HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC,        /* 声明能力，核心据此路由 */
    0,                                           /* priv_size：核心按此分配零化私有区 */
    my_configure, NULL, NULL, my_emit,           /* configure/init/start/emit */
    NULL, NULL, NULL, NULL,                      /* emit_batch/flush/sync/periodic */
    NULL, { NULL, NULL, NULL, NULL }             /* destroy + reserved（ABI 追加位） */
};

/* hpulogc_init() 之前： */
hpulogc_sink_register(&g_my_ops);
```

**per-sink 统计**（任一线程可读，事件视图仅回调内有效）：

```c
hpulogc_sink_stats_t st;
if (hpulogc_get_sink_stats("applog", &st) == HPULOGC_OK) {
    /* st.written / st.dropped（async 队列满）/ st.failed / st.bytes_written */
}
```

生命周期契约（create → configure* → init → start → emit/batch → flush
→ destroy）与 ABI 冻结规则的规范性描述见 `docs/rd_v0.6.md` §4.10。

## 安装与集成

```bash
cmake --install build --prefix /usr/local
```

```cmake
find_package(hpulogc REQUIRED)
target_link_libraries(myapp PRIVATE hpulogc::hpulogc)
```

## 文档

- 需求规格（规范性，唯一现行版）：`docs/rd_v0.6.md`（合并 v0.2 全文、
  v0.3 MPMC 增量与多 sink 体系；现行 v0.6.3 含网络型 sink §4.10.8 与
  错误描述 API `hpulogc_strerror` §7.3；历史版本 rd_v0.2/rd_v0.3 保留
  作快照）
- 多 sink 需求与设计：`docs/mul_sink.md`
- 用户决策记录：`docs/decision_log.md`
- 代码结构与平台契约：`docs/code_structure.md`
- 实现决策记录：`docs/implementation_notes.md`（含 Phase 2 Windows、
  Phase 4 MPMC、Phase 5 多 sink、网络型 sink 第一期章节与
  P-1..P-7 缺陷登记）
- 性能测试报告：`docs/perf_report.md`（Phase 1 / Linux）、
  `docs/perf_report_windows.md`（Phase 2 / Windows）、
  `docs/perf_report_sinks.md`（多 sink 分发开销）

## 配置校验 CLI

```bash
hpulogc_chk_conf [--strict|--lenient] <config.ini>
```

INI 构建自动编译（min 预设除外，§11）：校验通过退出码 0、非法 1、
用法/IO 错误 2；dry-run 校验不创建任何日志文件。CI 用它校验
`examples/conf_eg.ini`（`chk_conf_example` ctest）。程序内校验用
`hpulogc_conf_validate()`（§7.3）。

## 许可证

MIT（见 LICENSE）。
