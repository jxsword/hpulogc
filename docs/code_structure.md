# hpulogc 代码结构说明（Phase 1）

## 1. 目录树与逐文件职责

```
hpulogc/
├── include/
│   └── hpulogc.h                 公共 API（唯一对外头）：API 签名、枚举、
│                                 POD 结构、容量宏、版本宏、HPULOGC_API
│                                 导出宏、便捷宏（##__VA_ARGS__ 可移植序列、
│                                 HPULOGC_COMPILE_TIME_LEVEL 裁剪点）
├── src/
│   ├── atomic/
│   │   ├── hpulogc_atomic.h      统一原子接口：后端选择宏、usize 别名、
│   │   │                         hpu_cpu_relax()
│   │   ├── atomic_stdatomic.h    C11 <stdatomic.h> 后端（Linux/macOS C11）
│   │   ├── atomic_gcc.h          GCC/Clang __atomic_*（C99）与 __sync_*
│   │   │                         回退（HPULOGC_ATOMIC_BACKEND_GCC_SYNC）
│   │   └── atomic_msvc.h         MSVC Interlocked*（Windows；Phase 1 不编译）
│   ├── ring/
│   │   ├── ringbuf.h             环形缓冲内部契约：记录布局（48B 头）、
│   │   │                         put/get/close/counters API、view 结构
│   │   ├── ringbuf_locked.c      有锁实现（mutex+cond；SPSC/MPSC；
│   │   │                         discard/overwrite/wait 全策略）
│   │   └── ringbuf_lockfree.c    无锁实现（CAS 预订 + commit 标记发布；
│   │                             MPSC 仅 discard；SPSC 另支持 overwrite）
│   ├── platform/
│   │   ├── platform.h            契约伞头（含公共头引入容量宏）
│   │   ├── hpu_sync.h            mutex/cond 契约（有锁环依赖）
│   │   ├── hpu_thread.h          线程创建/join/atfork 契约
│   │   ├── hpu_time.h            高精度时钟 + 本地时间转换契约
│   │   ├── hpu_fs.h              文件 open/write/fsync/rename/unlink/
│   │   │                         目录扫描/mkdir/isatty/symlink 契约
│   │   ├── hpu_path.h            词法路径 helpers（dirname/basename/
│   │   │                         stem/normalize）契约
│   │   ├── hpu_watcher.h         配置文件变更监视契约（inotify/轮询）
│   │   ├── hpu_tid.h             系统线程 id 契约
│   │   ├── hpu_tls.h             线程局部存储（带析构）契约
│   │   ├── hpu_signal.h          SIGHUP 注册契约（热加载触发）
│   │   ├── posix/
│   │   │   ├── posix_sync.c      pthread mutex/cond（MONOTONIC condattr）
│   │   │   ├── posix_thread.c    pthread 封装 + pthread_atfork
│   │   │   ├── posix_time.c      clock_gettime/localtime_r/mktime
│   │   │   ├── posix_fs.c        open/write-all/fsync/readdir/mkdir_all
│   │   │   ├── posix_path.c      词法路径（'/' 语义）
│   │   │   ├── posix_tls.c       pthread_key 封装
│   │   │   ├── posix_tid.c       gettid（每线程缓存）
│   │   │   ├── posix_signal.c    SIGHUP handler（仅置 sig_atomic_t 标志）
│   │   │   ├── posix_watcher_poll.c  轮询回退后端（mtime+size 基线）
│   │   │   └── posix_watcher.c   非 Linux POSIX 的公共入口（→轮询）
│   │   ├── linux/
│   │   │   └── linux_watcher.c   inotify 后端（文件+父目录双 watch），
│   │   │                         inotify 不可用时降级轮询
│   │   ├── darwin/
│   │   │   ├── darwin_platform.h 契约快照（Phase 3 状态说明）
│   │   │   ├── darwin_sync.c     pthread 同步 + relative_np 相对期限
│   │   │   │                     （macOS condattr 仅支持 REALTIME）
│   │   │   └── darwin_tid.c      pthread_threadid_np 系统级线程 ID
│   │   └── win32/
│   │       ├── win32_platform.h  契约快照头（Phase 2 起附实现文件）
│   │       ├── win32_sync.c      CRITICAL_SECTION + CONDITION_VARIABLE
│   │       ├── win32_thread.c    _beginthreadex/join；atfork 空实现
│   │       ├── win32_time.c      QPC / GetSystemTimePreciseAsFileTime
│   │       │                     / localtime_s / _mktime64
│   │       ├── win32_fs.c        CreateFileW(UTF-8→UTF-16)/WriteFile/
│   │       │                     FlushFileBuffers/MoveFileExW/
│   │       │                     FindFirstFileW；权限忽略+一次性告警；
│   │       │                     VT 启用与 CRT 二进制模式
│   │       ├── win32_path.c      双分隔符 dirname/basename/stem/
│   │       │                     normalize（ASCII 大小写折叠）
│   │       ├── win32_watcher.c   恒轮询后端（GetFileAttributesExW
│   │       │                     mtime+size 基线）
│   │       ├── win32_tid.c       GetCurrentThreadId
│   │       ├── win32_tls.c       FlsAlloc 族（线程退出析构回调）
│   │       └── win32_signal.c    无 SIGHUP：install 恒 -1
│   ├── conf/
│   │   ├── ini.h                 zlog 词法解析器契约（1024B 物理行、
│   │   │                         注释/引号/续行）
│   │   ├── ini.c                 行式词法实现（file/buffer 双入口）
│   │   ├── conf_model.h          内部配置快照结构 + 终结化 API
│   │   ├── conf_model.c          默认值、代码内配置应用（严格校验）、
│   │   │                         finalize（引用解析/重复路径/naming/
│   │   │                         溢出策略 fail-fast/输出打开）、
│   │   │                         finalize_reload（fd 复用计划）
│   │   └── conf_file.c           配置文件解析（节顺序校验、§10.4 行为表、
│   │                             clamp+警告、[build] 只读比对、
│   │                             strict init 两遍扫描）
│   ├── format/
│   │   ├── format.h              预编译动作序列结构、渲染环境、记录结构
│   │   └── format.c              模板编译（热路径零二次解析）、渲染
│   │                             （时间按秒缓存、%f/%F3、JSON/注入转义、
│   │                             整行截断 + marker）
│   ├── output/
│   │   ├── output.h              输出层契约 + 后端声明 + fsync 严重度
│   │   ├── output.c              分发器（write/flush/sync/periodic）
│   │   ├── output_console.c      控制台后端（stdio、终端 ANSI 颜色）
│   │   ├── output_file.c         文件后端（批量写、失败重开一次、fsync
│   │                             策略、轮转检查钩子、lost 计数）
│   │   └── rotate.c              归档命名模板展开、时间桶边界
│   │                             （UTC 民历算法/本地 mktime）、冲突递增、
│   │                             max files 清理、.latest 符号链接
│   └── core/
│       │                         ★ Phase 2 重建（Phase 1 基线缺失本
│       │                         目录；依据规范+决策记录+既有测试断言
│       │                         重写，符号名与 Phase 1 文档一致，
│       │                         详见 implementation_notes Phase 2 节 A）
│       ├── core_internal.h       运行时状态、注册表/节流结构、内部 API
│       ├── core.c                生命周期（三入口 init/shutdown）、
│       │                         运行时控制 API、stats、build info、
│       │                         atfork（子进程锁重初始化）、fork 惰性重建
│       ├── api.c                 hpulogc_log/vlog 公共入口（errno 保持）
│       ├── pipeline.c            八步管线的生产者侧（过滤/节流/渲染/
│       │                         入队 或 同步直通）、TLS 渲染缓冲、
│       │                         路由+格式化+输出共享路径、令牌桶
│       ├── consumer.c            异步消费者线程（批量取出→路由→批量写、
│       │                         flush 握手、周期 fsync、统计输出）
│       ├── registry.c            类目登记表（count 发布协议、按级别
│       │                         缓存路由、每类目级别覆盖）
│       ├── hotreload.c           watcher 线程（inotify/SIGHUP/轮询）、
│       │                         do_reload 三阶段（校验→写锁搬迁→释放）
│       └── signal_safe.c         async-signal-safe 受限写通道
├── examples/
│   ├── minimal.c                 默认配置最小示例
│   ├── async.c                   代码配置 + 异步 + 批量 + shutdown 排空
│   ├── multi_category.c          多类目路由 + 每类目级别覆盖
│   └── custom_format.c           配置文件 + 自定义命名格式 + 轮转
├── tests/
│   ├── test_util.h               自注册 TEST 宏测试框架（头；
│   │                             MSVC 走 .CRT$XCU 段注册分支）
│   ├── portability.h             Phase 2：测试跨平台 shim（线程/屏障/
│   │                             睡眠/临时目录/rmtree/popen/QPC）
│   ├── check_windows_behavior.c  Phase 2：Windows 行为核查（手动运行）
│   ├── test_main.c               运行器实现 + main
│   ├── unit/
│   │   ├── test_ring.c           环形缓冲（4 构建组合 × 全策略、并发
│   │   │                         压力、会计恒等式；TSan 豁免见 notes）
│   │   ├── test_atomic.c         原子后端行为等价（3 后端分别编译）
│   │   ├── test_ini.c            词法逐条 + §10.4 行为表 + 全节解析
│   │   ├── test_format.c         占位符/时间扩展/转义/截断
│   │   ├── test_rotate.c         边界计算/命名/冲突/清理/软链
│   │   └── test_lifecycle.c      生命周期语义、stats、errno 保持、
│   │                             代码配置错误路径、build info
│   ├── integration/
│   │   ├── test_pipeline.c       §10.3.1 路由表逐行、兜底语义、JSON
│   │   │                         合法性（python 校验）、截断端到端、
│   │   │                         溢出会计、热加载（含回滚）
│   │   ├── test_fork.c           reinit/disable/inherit
│   │   ├── test_signal_safe.c    handler 内调用、开关门控、errno
│   │   └── test_diskfail.c       I/O 注入：写失败重开、lost 会计、
│   │                             init 打开失败 fail-fast
│   ├── fuzz/
│   │   ├── fuzz_ini.c            LLVMFuzzerTestOneInput harness
│   │   └── fuzz_main.c           确定性变异驱动（CTest 用）
│   └── bench/
│       ├── bench_log.c           延迟（逐条/摊销）+ 吞吐
│       └── bench_memory.c        min 基线内存（statm + mallinfo2）
├── scripts/
│   ├── run_matrix.sh             16 组合矩阵 + 4 预设一键验证（POSIX）
│   ├── run_matrix.ps1            Phase 2：{99,11}×{锁,无锁}×{SPSC,MPSC}
│   │                             8 组合 + 4 预设（MSVC，零告警门禁）
│   ├── msvc_build.bat            Phase 2：MSVC 环境包装（vcvars64）
│   └── msvc_env.sh               Phase 2：Git Bash 环境变量参考
├── cmake/
│   └── hpulogcConfig.cmake.in    find_package 包配置模板
├── CMakeLists.txt                全部选项/后端探测/sanitizer/预设/安装
├── AGENTS.md                     编码规范（规范性输入，未修改）
├── docs/                         rd_v0.1/0.2（规范性输入，未修改）+
│                                 本文件 + perf_report + implementation_notes
└── README.md                     快速开始
```

## 2. 平台契约冻结点清单（Phase 2/3 移植依据）

`src/platform/` 下的契约头一经冻结，Phase 2/3 不得要求修改：

| 契约头 | 职责 | win32 对应（Phase 2） | darwin 对应（Phase 3） |
|--------|------|----------------------|------------------------|
| hpu_sync.h | mutex/cond（值语义嵌入有锁环） | CRITICAL_SECTION/SRWLOCK + CONDITION_VARIABLE | 直接复用 posix 层 |
| hpu_thread.h | 线程创建/join、atfork 注册 | _beginthreadex；atfork 为空实现 | 复用 posix 层 |
| hpu_time.h | 单调/实时时钟、civil 时间转换 | QPC / GetSystemTimePreciseAsFileTime / localtime_s | 复用 posix 层（≥10.12） |
| hpu_fs.h | open/write-all/fsync/rename/unlink/stat/mkdir_all/isatty/symlink/list_dir/fchmod | CreateFile/WriteFile/FlushFileBuffers/MoveFileEx/FindFirstFile | 复用 posix 层 |
| hpu_path.h | dirname/basename/stem/normalize（词法） | '\' 语义 + UTF-8↔UTF-16 | 复用 posix 层 |
| hpu_watcher.h | 文件变更等待（后端自选 + 轮询回退） | 恒轮询 | kqueue 或轮询 |
| hpu_tid.h | 数值线程 id | GetCurrentThreadId | pthread_threadid_np |
| hpu_tls.h | 带析构的 TLS 槽 | TlsAlloc 族 | 复用 posix 层 |
| hpu_signal.h | SIGHUP 安装 + 原子取清 | 无（恒 -1） | 复用 posix 层 |

动态库导出属性在公共头 `HPULOGC_API`（GCC/Clang visibility + Windows
dllexport/dllimport），不属平台层。

## 3. Phase 2/3 实施状态（Phase 2 已交付）

- `src/platform/win32/`：✅ 全部契约实现完成（9 个 .c 文件 + 快照
  头），CMake WIN32 分支接入。契约冻结清单核对：本目录仅实现契约
  声明的函数，未新增/修改任何契约签名（hpu_sync.h 的 WIN32 结构体
  分支为 Phase 1 预定的 Phase 2 填充点）。实现要点与 Windows 特有
  行为决策见 implementation_notes.md 的 Phase 2 章节。
- `src/atomic/atomic_msvc.h`：✅ 启用并验证——MSVC 与 MinGW-w64
  均走 `HPULOGC_ATOMIC_BACKEND_MSVC`（Windows 后端唯一，§4.3）；
  Phase 2 修正 MinGW 兼容（_ReadBarrier）与 32 位目标 64 原子读写。
- `docs/perf_report_windows.md`：✅ 五指标实测 + 与 Phase 1 同口径
  对比 + min 体积（20,428 B）。
- `src/platform/darwin/`：✅ Phase 3 已交付并经 **macOS CI 全绿**
  验证（GitHub Actions 矩阵，见 implementation_notes.md Phase 3
  章节 E）——`darwin_sync.c`（macOS condattr 仅支持 REALTIME，
  timedwait 改 relative_np）与 `darwin_tid.c`
  （pthread_threadid_np）；watcher 按 §16.3 选择轮询回退（kqueue
  为可选增强，遗留）；其余复用 posix 层。Apple Clang 双标准
  （C11 stdatomic / C99 __atomic_*）、双架构（arm64/x86_64-
  Rosetta）与 ASan 均在 CI 矩阵中实测通过。

## 4. CMake 选项与四预设

### 功能裁剪（§4.8）

| 选项 | 默认 | 说明 |
|------|------|------|
| HPULOGC_ENABLE_ASYNC | ON | 消费者线程 + 批量提交 |
| HPULOGC_ENABLE_COLOR | ON | ANSI 颜色（仅终端） |
| HPULOGC_ENABLE_ROTATE | ON | 轮转（OFF 时 rotate.c 移出构建） |
| HPULOGC_ENABLE_INI | ON | INI 解析（OFF 时 ini.c/conf_file.c 移出，init_from_file 返回 ERR_CONFIG） |
| HPULOGC_ENABLE_HOT_RELOAD | ON | 配置热加载 |
| HPULOGC_ENABLE_CATEGORY | ON | 多类目路由与 per-category 级别 |
| HPULOGC_ENABLE_THROTTLE | OFF | 限流与采样 |
| HPULOGC_ENABLE_SOURCE_LOC | ON | __FILE__/__LINE__ 捕获 |
| HPULOGC_LOCKFREE | OFF | 环形缓冲实现二选一 |
| HPULOGC_CONCURRENCY | MPSC | SPSC / MPSC |
| HPULOGC_COMPILE_TIME_LEVEL | TRACE | 便捷宏裁剪阈值（0..6，PUBLIC 宏传播给使用者） |

### 构建控制

| 选项 | 值 |
|------|----|
| HPULOGC_C_STANDARD | 99 / 11（默认 99，gnu 扩展开启以支持 ##\_\_VA_ARGS\_\_） |
| HPULOGC_ATOMIC_BACKEND | auto / stdatomic / gcc-atomic / gcc-sync / msvc-interlocked（auto 按 C 标准与编译器探测；交叉指定报错） |
| HPULOGC_SANITIZER | none / address / thread / undefined / address,undefined（thread 互斥） |
| HPULOGC_BUILD_PRESET | full / min / sync_thread / async_single / custom（预设强制项覆盖用户选项并告警） |
| HPULOGC_MIN_SIZE_OPT | min 预设自动置 ON（-Os + 段 GC） |
| HPULOGC_WERROR | 警告升级 |
| HPULOGC_BUILD_TESTS / BUILD_EXAMPLES / BUILD_SHARED | 开关 |

### 预设（§5）

| 预设 | 强制项 |
|------|--------|
| full | 默认（全功能，MPSC+有锁） |
| min | ASYNC/COLOR/ROTATE/CATEGORY/HOT_RELOAD/THROTTLE/INI=OFF，SPSC+有锁，-Os |
| sync_thread | ASYNC=OFF，LOCKFREE=OFF（其余默认） |
| async_single | ASYNC=ON，LOCKFREE=ON，SPSC |

### 使用示例

```bash
# 默认全功能 + 测试
cmake -S . -B build -G Ninja && ninja -C build && (cd build && ctest)

# 无锁 SPSC + C11 + ASan
cmake -S . -B build/asan -G Ninja -DHPULOGC_LOCKFREE=ON \
  -DHPULOGC_CONCURRENCY=SPSC -DHPULOGC_C_STANDARD=11 \
  -DHPULOGC_SANITIZER=address,undefined

# 全矩阵（16 组合 + 4 预设）
bash scripts/run_matrix.sh

# min 体积验证
cmake -S . -B build/min -G Ninja -DHPULOGC_BUILD_PRESET=min
```

### 安装

`cmake --install` 安装 `include/hpulogc.h`、静态/动态库、
`hpulogcConfig.cmake` / `hpulogcConfigVersion.cmake` / 导出目标
（`find_package(hpulogc)` → `hpulogc::hpulogc`）。
