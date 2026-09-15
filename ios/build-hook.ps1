chcp 65001 > $null
$ErrorActionPreference = 'Stop'
$env:PYTHONIOENCODING = 'utf-8'
$taskRoot = $PSScriptRoot
$taskRepo = Split-Path $taskRoot
$taskLlvm = Join-Path $env:LOCALAPPDATA 'Android/Sdk/ndk/26.3.11579264/toolchains/llvm/prebuilt/windows-x86_64/bin'
if ($env:HOSHIMI_LLVM_BIN) { $taskLlvm = $env:HOSHIMI_LLVM_BIN }
$taskOut = Join-Path $taskRoot 'build/hook-v2'
python (Join-Path $taskRoot 'tools/prepare_hook.py') `
    --ipa (Join-Path $taskRepo 'dump/ios/game.qualiarts.idolypride-6.0.2-Decrypted.ipa') `
    --translations (Join-Path $taskRepo 'app/src/main/assets/hoshimi-local/local-files/localization.json') --out $taskOut
if ($LASTEXITCODE -ne 0) { throw 'Hook input validation failed' }
& (Join-Path $taskLlvm 'clang.exe') -target arm64-apple-ios13.0 -DHOSHIMI_SDK_FREE `
    -ffreestanding -fno-stack-protector -fvisibility=hidden -O2 -Wall -Wextra -Werror `
    -I $taskOut -c (Join-Path $taskRoot 'hook/hook.c') -o (Join-Path $taskOut 'hook.o')
if ($LASTEXITCODE -ne 0) { throw 'ARM64 compilation failed' }
& (Join-Path $taskLlvm 'clang.exe') -target arm64-apple-ios13.0 -DHOSHIMI_SDK_FREE `
    -ffreestanding -fno-stack-protector -fvisibility=hidden -O2 -Wall -Wextra -Werror `
    -c (Join-Path $taskRoot 'probe/probe.c') -o (Join-Path $taskOut 'probe.o')
if ($LASTEXITCODE -ne 0) { throw 'ARM64 diagnostic compilation failed' }
& (Join-Path $taskLlvm 'ld.lld.exe') -flavor darwin -dylib -arch arm64 `
    -platform_version ios 13.0 13.0 -install_name '@rpath/HoshimiLocalify.dylib' `
    -adhoc_codesign (Join-Path $taskOut 'hook.o') (Join-Path $taskOut 'probe.o') `
    (Join-Path $taskRoot 'hook/libSystem.tbd') `
    -o (Join-Path $taskOut 'HoshimiLocalify.dylib')
if ($LASTEXITCODE -ne 0) { throw 'Mach-O linking failed' }
Write-Output "Built: $taskOut/HoshimiLocalify.dylib"
