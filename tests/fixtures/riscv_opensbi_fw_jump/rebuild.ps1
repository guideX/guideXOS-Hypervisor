param(
    [string]$OpenSbiSource = "",
    [string]$MsysRoot = "C:\msys64",
    [string]$LlvmBin = "C:\Program Files\LLVM\bin"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$fixture = $PSScriptRoot
$buildRoot = Join-Path $fixture ".build"
$toolsRoot = Join-Path $buildRoot "host-tools"
$opensbiCommit = "74434f255873d74e56cc50aa762d1caf24c099f8"
$opensbiUrl = "https://github.com/riscv-software-src/opensbi.git"
$bash = Join-Path $MsysRoot "usr\bin\bash.exe"
$cygpath = Join-Path $MsysRoot "usr\bin\cygpath.exe"
$make = Join-Path $MsysRoot "usr\bin\make.exe"
$dtc = Join-Path $MsysRoot "usr\bin\dtc.exe"
$clang = Join-Path $LlvmBin "clang.exe"
$objcopy = Join-Path $LlvmBin "llvm-objcopy.exe"
$readobj = Join-Path $LlvmBin "llvm-readobj.exe"
$objdump = Join-Path $LlvmBin "llvm-objdump.exe"

foreach ($tool in @($bash, $cygpath, $make, $dtc, $clang, $objcopy, $readobj, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required build tool not found: $tool" }
}

if ([string]::IsNullOrWhiteSpace($OpenSbiSource)) {
    $OpenSbiSource = Join-Path $buildRoot "opensbi-v1.8.1"
    if (-not (Test-Path -LiteralPath (Join-Path $OpenSbiSource ".git"))) {
        New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
        & git clone --depth 1 --branch v1.8.1 $opensbiUrl $OpenSbiSource
        if ($LASTEXITCODE -ne 0) { throw "Could not clone OpenSBI v1.8.1" }
    }
}
$OpenSbiSource = (Resolve-Path -LiteralPath $OpenSbiSource).Path

$actualCommit = (& git -C $OpenSbiSource rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $opensbiCommit) {
    throw "OpenSBI source must be commit $opensbiCommit; found $actualCommit"
}

New-Item -ItemType Directory -Path $buildRoot, $toolsRoot -Force | Out-Null
$clangWrapperPath = Join-Path $toolsRoot "clang"
$pythonWrapperPath = Join-Path $toolsRoot "python3"
$probeSource = Join-Path $buildRoot "probe-empty.c"
$probeOutput = Join-Path $buildRoot "probe-output.elf"
Set-Content -LiteralPath $probeSource -Value "" -NoNewline

function Convert-ToMsysPath([string]$Path) {
    $result = & $cygpath -u $Path
    if ($LASTEXITCODE -ne 0) { throw "Could not translate path for MSYS2: $Path" }
    return $result.Trim()
}

$clangWrapper = @'
#!/usr/bin/env bash
set -u
real_clang='__REAL_CLANG__'
probe_input='__PROBE_INPUT__'
probe_output='__PROBE_OUTPUT__'
args=("$@")
for ((i=0; i<${#args[@]}; ++i)); do
    if [[ "${args[$i]}" == '/dev/null' ]]; then
        if ((i > 0)) && [[ "${args[$((i-1))]}" == '-o' ]]; then
            args[$i]="$probe_output"
        else
            args[$i]="$probe_input"
        fi
    fi
done
"$real_clang" "${args[@]}"
status=$?
rm -f "$probe_output"
exit "$status"
'@
$clangWrapper = $clangWrapper.Replace("__REAL_CLANG__", (Convert-ToMsysPath $clang))
$clangWrapper = $clangWrapper.Replace("__PROBE_INPUT__", (Convert-ToMsysPath $probeSource))
$clangWrapper = $clangWrapper.Replace("__PROBE_OUTPUT__", (Convert-ToMsysPath $probeOutput))
[IO.File]::WriteAllText($clangWrapperPath, $clangWrapper, [Text.UTF8Encoding]::new($false))

$pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
if ($null -eq $pythonCommand) { throw "Python 3 is required for the OpenSBI host build" }
$pythonWrapper = @'
#!/usr/bin/env bash
exec '__PYTHON_EXE__' "$@"
'@
$pythonWrapper = $pythonWrapper.Replace("__PYTHON_EXE__", (Convert-ToMsysPath $pythonCommand.Source))
[IO.File]::WriteAllText($pythonWrapperPath, $pythonWrapper, [Text.UTF8Encoding]::new($false))

$sourcePosix = Convert-ToMsysPath $OpenSbiSource
$toolsPosix = Convert-ToMsysPath $toolsRoot
$llvmPosix = Convert-ToMsysPath $LlvmBin
$wrapperPosix = Convert-ToMsysPath $clangWrapperPath
$pythonPosix = Convert-ToMsysPath $pythonWrapperPath
$makeCommand = "chmod +x '$wrapperPosix' '$pythonPosix'; export PATH='$toolsPosix`:$llvmPosix`:/usr/bin`:`$PATH'; cd '$sourcePosix'; make -j4 LLVM=1 PLATFORM=generic FW_JUMP=y FW_DYNAMIC=n FW_PAYLOAD=n FW_TEXT_START=0x80000000 FW_JUMP_ADDR=0x80200000 FW_JUMP_FDT_OFFSET=0x2200000 PLATFORM_RISCV_XLEN=64 PLATFORM_RISCV_ABI=lp64 PLATFORM_RISCV_ISA=rv64ima_zicsr REPRODUCIBLE=1"
& $bash -lc $makeCommand
if ($LASTEXITCODE -ne 0) { throw "OpenSBI generic FW_JUMP build failed" }

$firmwareDir = Join-Path $OpenSbiSource "build\platform\generic\firmware"
Copy-Item -LiteralPath (Join-Path $firmwareDir "fw_jump.bin") -Destination (Join-Path $fixture "fw_jump.bin") -Force
Copy-Item -LiteralPath (Join-Path $firmwareDir "fw_jump.elf") -Destination (Join-Path $fixture "fw_jump.elf") -Force

& $clang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax `
    -ffreestanding -fno-stack-protector -nostdlib `
    "-Wl,-T,$(Join-Path $fixture 'supervisor.ld')" `
    "-Wl,-Map,$(Join-Path $buildRoot 'supervisor.map')" `
    -o (Join-Path $buildRoot "supervisor.elf") (Join-Path $fixture "supervisor.S")
if ($LASTEXITCODE -ne 0) { throw "Supervisor payload build failed" }
& $objcopy -S -O binary (Join-Path $buildRoot "supervisor.elf") (Join-Path $buildRoot "supervisor.bin")
if ($LASTEXITCODE -ne 0) { throw "Supervisor payload binary extraction failed" }
Copy-Item -LiteralPath (Join-Path $buildRoot "supervisor.elf") -Destination (Join-Path $fixture "supervisor.elf") -Force
Copy-Item -LiteralPath (Join-Path $buildRoot "supervisor.bin") -Destination (Join-Path $fixture "supervisor.bin") -Force

& $dtc -I dts -O dtb -o (Join-Path $buildRoot "platform.dtb") (Join-Path $fixture "platform.dts")
if ($LASTEXITCODE -ne 0) { throw "FDT compilation failed" }
Copy-Item -LiteralPath (Join-Path $buildRoot "platform.dtb") -Destination (Join-Path $fixture "platform.dtb") -Force

$elfInfo = & $readobj --file-headers --program-headers --sections --symbols (Join-Path $fixture "supervisor.elf") | Out-String
$disassembly = & $objdump --disassemble (Join-Path $fixture "supervisor.elf") | Out-String
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-elf-info.txt"), $elfInfo, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-disassembly.txt"), $disassembly, [Text.UTF8Encoding]::new($false))

$hashLines = foreach ($name in @("fw_jump.bin", "fw_jump.elf", "supervisor.bin", "supervisor.elf", "platform.dtb")) {
    $item = Get-Item -LiteralPath (Join-Path $fixture $name)
    $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
    "{0}  {1}  {2} bytes" -f $hash, $name, $item.Length
}
$manifestHeader = @(
    "OpenSBI source: $opensbiUrl",
    "OpenSBI tag: v1.8.1",
    "OpenSBI commit: $actualCommit",
    "OpenSBI config: PLATFORM=generic FW_JUMP=y FW_DYNAMIC=n FW_PAYLOAD=n FW_TEXT_START=0x80000000 FW_JUMP_ADDR=0x80200000 FW_JUMP_FDT_OFFSET=0x2200000 PLATFORM_RISCV_XLEN=64 PLATFORM_RISCV_ABI=lp64 PLATFORM_RISCV_ISA=rv64ima_zicsr REPRODUCIBLE=1 LLVM=1",
    "RV64 supervisor flags: --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax -ffreestanding -fno-stack-protector -nostdlib",
    ""
)
$manifest = @($manifestHeader + $hashLines) -join "`r`n"
[IO.File]::WriteAllText((Join-Path $fixture "binary-manifest.txt"), $manifest + "`r`n", [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath (Join-Path $fixture "binary-manifest.txt")
