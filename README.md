# hpulogc — High Performance Log in C

纯 C99/C11、零第三方依赖、跨平台的高性能日志库。

- **无锁环形缓冲**（SPSC/MPSC，discard/overwrite 溢出策略）与有锁实现二选一
- **异步消费者**线程 + 批量同步落盘（实测 ~5.2M logs/sec @64B，Windows 原生）
- **INI 配置**（zlog 兼容词法）+ 热加载（inotify/SIGHUP/轮询）+ 原子替换回滚
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

或使用 INI 配置文件（模板见 `docs/rd_v0.2.md` §10.3）：

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
- 配置文件热加载为轮询后端（kqueue 为可选增强，§16.3）；
- 双架构（x86_64/ARM64）与 ASan/TSan 由 GitHub Actions 矩阵覆盖。

常用选项（全平台一致）：

```bash
-DHPULOGC_BUILD_PRESET=min          # 嵌入式精简版（无配置文件/轮转/颜色）
-DHPULOGC_BUILD_PRESET=async_single # 无锁 SPSC 高吞吐版
-DHPULOGC_LOCKFREE=ON               # 无锁环形缓冲
-DHPULOGC_CONCURRENCY=SPSC          # 单生产者模式
-DHPULOGC_C_STANDARD=11             # C11（默认 99）
-DHPULOGC_ATOMIC_BACKEND=gcc-atomic # 强制原子后端
-DHPULOGC_SANITIZER=address         # address / thread / undefined / address,undefined
```

全矩阵验证：

```bash
# Linux / macOS（GCC/Clang × C99/C11 × 有锁/无锁 × SPSC/MPSC + 4 预设）
bash scripts/run_matrix.sh

# Windows MSVC（C99/C11 × 有锁/无锁 × SPSC/MPSC + 4 预设，含零告警门禁）
powershell -ExecutionPolicy Bypass -File scripts\run_matrix.ps1
```

## 安装与集成

```bash
cmake --install build --prefix /usr/local
```

```cmake
find_package(hpulogc REQUIRED)
target_link_libraries(myapp PRIVATE hpulogc::hpulogc)
```

## 文档

- 需求规格：`docs/rd_v0.2.md`（规范性）
- 代码结构与平台契约：`docs/code_structure.md`
- 实现决策记录：`docs/implementation_notes.md`（含 Phase 2 Windows 章节）
- 性能测试报告：`docs/perf_report.md`（Phase 1 / Linux）、
  `docs/perf_report_windows.md`（Phase 2 / Windows）

## 许可证

MIT（见 LICENSE）。
