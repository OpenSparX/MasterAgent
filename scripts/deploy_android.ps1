param(
    [Parameter(Mandatory = $true)]
    [string]$AdbSerial,

    [Parameter(Mandatory = $true)]
    [string]$NdkRoot,

    [string]$DeviceRoot = "/data/local/tmp/master-agent",
    [string]$BuildDirectory = "build-android-arm64",
    [switch]$RunSmokeTest
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$sourceRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $sourceRoot $BuildDirectory
$deployRoot = Join-Path $buildRoot "deploy"
$toolchain = Join-Path $NdkRoot "build/cmake/android.toolchain.cmake"
$strip = Join-Path $NdkRoot "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"

if (-not (Test-Path -LiteralPath $toolchain)) {
    throw "Android NDK toolchain was not found: $toolchain"
}

$ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $ninja) {
    $ninja = Get-ChildItem `
        "$env:LOCALAPPDATA/Microsoft/WinGet/Packages" `
        -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "Ninja-build\.Ninja" } |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $ninja) {
    throw "Ninja was not found"
}

& cmake -S $sourceRoot -B $buildRoot -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DANDROID_ABI=arm64-v8a" `
    "-DANDROID_PLATFORM=android-28" `
    "-DANDROID_STL=c++_static" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DMASTER_AGENT_BUILD_APP=ON" `
    "-DMASTER_AGENT_BUILD_TESTS=ON" `
    "-DMASTER_AGENT_ENABLE_IPC=OFF"

& cmake --build $buildRoot --parallel 6

New-Item -ItemType Directory -Path $deployRoot -Force | Out-Null
$artifacts = @(
    @{ Source = "master_agent"; Destination = "master_agent" },
    @{ Source = "tests/test_e2e"; Destination = "test_e2e" },
    @{ Source = "tests/test_atomic"; Destination = "test_atomic" },
    @{ Source = "tests/test_inference"; Destination = "test_inference" },
    @{ Source = "tests/test_observability"; Destination = "test_observability" },
    @{ Source = "tests/test_resilience"; Destination = "test_resilience" },
    @{ Source = "tests/test_contracts"; Destination = "test_contracts" },
    @{ Source = "tests/test_mcp_wire_contracts"; Destination = "test_mcp_wire_contracts" }
)

foreach ($artifact in $artifacts) {
    $source = Join-Path $buildRoot $artifact.Source
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Expected build artifact was not found: $source"
    }
    $destination = Join-Path $deployRoot $artifact.Destination
    Copy-Item -LiteralPath $source -Destination $destination -Force
    & $strip --strip-unneeded $destination
}

# Deployment is intentionally scoped to the explicitly selected device root.
# The script does not delete or alter any sibling device directory.
& adb -s $AdbSerial shell `
    "mkdir -p $DeviceRoot/bin $DeviceRoot/run && chmod 700 $DeviceRoot $DeviceRoot/bin $DeviceRoot/run"
& adb -s $AdbSerial push "$deployRoot/." "$DeviceRoot/bin/"
& adb -s $AdbSerial shell "chmod 700 $DeviceRoot/bin/*"

if ($RunSmokeTest) {
    & adb -s $AdbSerial shell `
        "cd $DeviceRoot/bin && ./master_agent --runtime=$DeviceRoot/run '请把空调切换到内循环'"
}
