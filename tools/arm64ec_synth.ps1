# Builds the VST3 test synth as ARM64EC, and as ARM64X when the arm64 build is beside it, with
# Visual Studio's ARM64 and ARM64EC build tools, natively on an ARM64 Windows or cross on an x64
# one (docs/development.md):
#   tools\arm64ec_synth.ps1 [-Arm64Build build-arm64]
# ARM64EC code runs natively in an x64 process on an ARM64 PC, which loads it from a VST3 bundle's
# Contents\arm64ec-win (or arm64x-win) before Contents\x86_64-win (plugin_files.cpp). Each bundle
# lands in a folder of its own, as tools/arch_matrix.sh and test_dll take a build's bin folder:
#   build-arm64ec\arm64ec-win\brack-test-synth.vst3\Contents\arm64ec-win\brack-test-synth.vst3
#   build-arm64ec\arm64x-win\brack-test-synth.vst3\Contents\arm64x-win\brack-test-synth.vst3
# The ARM64EC half is built by CMake in build-arm64ec (only the synth and the VST3 SDK), with the
# arm64 compiler's /arm64EC; MSVC's lib\ARM64 libraries are ARM64X, so they serve both links. The
# ARM64X bundle links the arm64 build's objects of the same sources with the ARM64EC ones.
param([string]$Arm64Build = "build-arm64")
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [Console]::InputEncoding = [Text.Encoding]::UTF8
$repo = Split-Path $PSScriptRoot
Set-Location $repo

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest `
    -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64EC -property installationPath
if (-not $vs) { throw "no Visual Studio with the ARM64EC build tools (Microsoft.VisualStudio.Component.VC.Tools.ARM64EC)" }
# Windows' own name for its CPU: this PowerShell may be an x64 one under emulation.
$cpu = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment").PROCESSOR_ARCHITECTURE
$target = if ($cpu -eq "ARM64") { "arm64" } else { "amd64_arm64" }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" $target >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

# CMake's own flags for MSVC (as in the arm64 build's cache) and /arm64EC: given flags replace them.
$flags = "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /arm64EC", "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /arm64EC", "-DCMAKE_SHARED_LINKER_FLAGS=/MACHINE:ARM64EC",
         "-DCMAKE_MODULE_LINKER_FLAGS=/MACHINE:ARM64EC", "-DCMAKE_EXE_LINKER_FLAGS=/MACHINE:ARM64EC", "-DBRACK_BUILD_GUI=OFF"
cmake -S . -B build-arm64ec -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo @flags
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build build-arm64ec --target brack_test_synth_vst3
if ($LASTEXITCODE) { exit $LASTEXITCODE }

function Bundle([string]$folder, [string]$binary) {
    $bundle = "build-arm64ec\$folder\brack-test-synth.vst3"
    Remove-Item -Recurse -Force $bundle -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force "$bundle\Contents\$folder" | Out-Null
    Copy-Item $binary "$bundle\Contents\$folder\brack-test-synth.vst3"
    Write-Output "$bundle\Contents\$folder"
}
Bundle "arm64ec-win" "build-arm64ec\bin\brack-test-synth.vst3"

$obj = "tests\CMakeFiles\brack_test_synth_vst3.dir\test_vst3\test_vst3_synth.cpp.obj"
if (-not (Test-Path "$Arm64Build\$obj")) {
    Write-Output "no arm64 build of the synth in $Arm64Build, so no ARM64X bundle"
    exit 0
}
$ecLib = Get-ChildItem build-arm64ec -Recurse -Filter brack_dep_vst3.lib | Select-Object -First 1
$a64Lib = Get-ChildItem $Arm64Build -Recurse -Filter brack_dep_vst3.lib | Select-Object -First 1
link /nologo /DLL /MACHINE:ARM64X /DEF:tests\test_vst3\test_vst3_synth.def /DEFARM64NATIVE:tests\test_vst3\test_vst3_synth.def `
    /OUT:build-arm64ec\brack-test-synth-arm64x.vst3 "$Arm64Build\$obj" "build-arm64ec\$obj" $a64Lib.FullName $ecLib.FullName ole32.lib
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Bundle "arm64x-win" "build-arm64ec\brack-test-synth-arm64x.vst3"
