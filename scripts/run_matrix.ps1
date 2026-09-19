# ============================================================================
# scripts/run_matrix.ps1 - hpulogc Phase 2 Windows verification matrix
#
# Builds and runs the full CTest suite for:
#   - functional matrix: {C99, C11} x {locked, lockfree} x {SPSC, MPSC}
#     (8 combinations on the single Windows toolchain, spec 13.5)
#   - the four build presets (spec 5)
#
# Usage:
#   powershell -File scripts\run_matrix.ps1              # matrix + presets
#   powershell -File scripts\run_matrix.ps1 -Quick       # matrix only
#   powershell -File scripts\run_matrix.ps1 -Compiler "D:\VS2026\VC\Auxiliary\Build\vcvars64.bat"
# ============================================================================

param(
    [switch]$Quick,
    [string]$VcVars = "D:\VS2026\VC\Auxiliary\Build\vcvars64.bat",
    [string]$Ninja = "D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Out = Join-Path $Root "build\matrix"

$script:Pass = 0
$script:Fail = 0
$script:FailedNames = @()

function Run-Combo([string]$Name, [string[]]$CmakeArgs) {
    $Dir = Join-Path $Out $Name
    Write-Host "==> [$Name] $($CmakeArgs -join ' ')"

    $cfgLog = "$Dir.cfg.log"
    $buildLog = "$Dir.build.log"
    $testLog = "$Dir.test.log"
    $wrapper = Join-Path $Root "scripts\msvc_build.bat"

    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $cfgCmd = "call `"$wrapper`" cmake -S `"$Root`" -B `"$Dir`" -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=Release $($CmakeArgs -join ' ') > `"$cfgLog`" 2>&1"
    cmd /c $cfgCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    CONFIGURE FAILED (see $cfgLog)"
        $script:Fail++; $script:FailedNames += "$Name(cfg)"; return
    }
    cmd /c "call `"$wrapper`" cmake --build `"$Dir`" > `"$buildLog`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    BUILD FAILED (see $buildLog)"
        $script:Fail++; $script:FailedNames += "$Name(build)"; return
    }
    # zero-warning gate: library + tests + examples compile clean
    $warns = Select-String -Path $buildLog -Pattern "warning C" -ErrorAction SilentlyContinue
    if ($warns) {
        Write-Host "    WARNINGS PRESENT ($($warns.Count) lines)"
        $script:Fail++; $script:FailedNames += "$Name(warnings)"; return
    }
    cmd /c "call `"$wrapper`" ctest --test-dir `"$Dir`" --output-on-failure > `"$testLog`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    TEST FAILED (see $testLog)"
        $script:Fail++; $script:FailedNames += "$Name(test)"; return
    }
    Write-Host "    PASS"
    $script:Pass++
}

# ---------------------------------------------------------------------------
# Functional matrix: {C99, C11} x {locked, lockfree} x {SPSC, MPSC}
# ---------------------------------------------------------------------------
Write-Host "==== functional matrix: {99,11} x {locked,lockfree} x {SPSC,MPSC} ===="
foreach ($std in @("99", "11")) {
    foreach ($lockfree in @("OFF", "ON")) {
        foreach ($conc in @("SPSC", "MPSC")) {
            $lfTag = if ($lockfree -eq "ON") { "lf" } else { "lk" }
            $name = "msvc_c$std`_$lfTag`_$conc"
            Run-Combo $name @(
                "-DHPULOGC_C_STANDARD=$std",
                "-DHPULOGC_LOCKFREE=$lockfree",
                "-DHPULOGC_CONCURRENCY=$conc",
                "-DHPULOGC_BUILD_EXAMPLES=OFF"
            )
        }
    }
}

if (-not $Quick) {
    Write-Host "==== build presets (spec 5) ===="
    foreach ($preset in @("full", "min", "sync_thread", "async_single")) {
        Run-Combo "preset_$preset" @(
            "-DHPULOGC_BUILD_PRESET=$preset",
            "-DHPULOGC_BUILD_EXAMPLES=OFF"
        )
    }
}

Write-Host "===="
Write-Host "matrix results: $($script:Pass) passed, $($script:Fail) failed"
if ($script:Fail -ne 0) {
    Write-Host "failed: $($script:FailedNames -join ' ')"
    exit 1
}
exit 0
