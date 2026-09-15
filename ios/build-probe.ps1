chcp 65001 > $null
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskLlvm = Join-Path $env:LOCALAPPDATA 'Android/Sdk/ndk/26.3.11579264/toolchains/llvm/prebuilt/windows-x86_64/bin'
if ($env:HOSHIMI_LLVM_BIN) { $taskLlvm = $env:HOSHIMI_LLVM_BIN }
$taskOut = Join-Path $taskRoot 'build'
New-Item -ItemType Directory -Path $taskOut -Force | Out-Null
& (Join-Path $taskLlvm 'clang.exe') -target arm64-apple-ios13.0 -DHOSHIMI_SDK_FREE `
    -ffreestanding -fno-stack-protector -fvisibility=hidden -O2 -Wall -Wextra -Werror `
    -c (Join-Path $taskRoot 'probe/probe.c') -o (Join-Path $taskOut 'probe.o')
if ($LASTEXITCODE -ne 0) { throw 'ARM64 compilation failed' }
& (Join-Path $taskLlvm 'ld.lld.exe') -flavor darwin -dylib -arch arm64 `
    -platform_version ios 13.0 13.0 -install_name '@rpath/HoshimiProbe.dylib' `
    -adhoc_codesign (Join-Path $taskOut 'probe.o') (Join-Path $taskRoot 'probe/libSystem.tbd') `
    -o (Join-Path $taskOut 'HoshimiProbe.dylib')
if ($LASTEXITCODE -ne 0) { throw 'Mach-O linking failed' }
Write-Output "Built: $taskOut/HoshimiProbe.dylib (SideStore re-signing required)"
