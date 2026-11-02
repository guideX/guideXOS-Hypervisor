param(
    [string]$MsysRoot = "C:\msys64",
    [string]$LlvmBin = "C:\Program Files\LLVM\bin"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$fixture = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $fixture "..\..\..\")).Path
$buildRoot = Join-Path $fixture ".build"
$llvmClang = Join-Path $LlvmBin "clang.exe"
$objcopy = Join-Path $LlvmBin "llvm-objcopy.exe"
$readobj = Join-Path $LlvmBin "llvm-readobj.exe"
$objdump = Join-Path $LlvmBin "llvm-objdump.exe"
$dtc = Join-Path $MsysRoot "usr\bin\dtc.exe"
$firmware = Join-Path $repo "tests\fixtures\riscv_opensbi_fw_jump\fw_jump.bin"
$firmwareSha = "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3"

foreach ($tool in @($llvmClang, $objcopy, $readobj, $objdump, $dtc)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool not found: $tool" }
}
if ((Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash -ne $firmwareSha) {
    throw "The accepted RISCV-9 OpenSBI firmware hash does not match."
}

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$elfBuild = Join-Path $buildRoot "supervisor.elf"
$binBuild = Join-Path $buildRoot "supervisor.bin"
$dtbBuild = Join-Path $buildRoot "platform.dtb"
& $llvmClang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax `
    -ffreestanding -fno-stack-protector -nostdlib `
    "-Wl,-T,$(Join-Path $fixture 'supervisor.ld')" `
    "-Wl,-Map,$(Join-Path $buildRoot 'supervisor.map')" `
    -o $elfBuild (Join-Path $fixture "supervisor.S")
if ($LASTEXITCODE -ne 0) { throw "Timer supervisor payload build failed" }
& $objcopy -S -O binary $elfBuild $binBuild
if ($LASTEXITCODE -ne 0) { throw "Timer supervisor binary extraction failed" }
& $dtc -I dts -O dtb -o $dtbBuild (Join-Path $fixture "platform.dts")
if ($LASTEXITCODE -ne 0) { throw "Timer FDT compilation failed" }

Copy-Item -LiteralPath $elfBuild -Destination (Join-Path $fixture "supervisor.elf") -Force
Copy-Item -LiteralPath $binBuild -Destination (Join-Path $fixture "supervisor.bin") -Force
Copy-Item -LiteralPath $dtbBuild -Destination (Join-Path $fixture "platform.dtb") -Force
$elfInfo = & $readobj --file-headers --program-headers --sections --symbols $elfBuild | Out-String
$disassembly = & $objdump --disassemble $elfBuild | Out-String
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-elf-info.txt"), $elfInfo, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-disassembly.txt"), $disassembly, [Text.UTF8Encoding]::new($false))
$version = (& $llvmClang --version | Select-Object -First 1).Trim()
$dtcVersion = (& $dtc --version | Select-Object -First 1).Trim()
$hashLines = foreach ($name in @("supervisor.bin", "supervisor.elf", "platform.dtb")) {
    $file = Join-Path $fixture $name
    "{0}  {1}  {2} bytes" -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash,
        $name, (Get-Item -LiteralPath $file).Length
}
$manifestLines = @(
    "OpenSBI provenance: ../riscv_opensbi_fw_jump/binary-manifest.txt",
    "OpenSBI source commit: 74434f255873d74e56cc50aa762d1caf24c099f8",
    "Reused OpenSBI fw_jump.bin SHA-256: $firmwareSha",
    "Payload compiler: $version",
    "DTC: $dtcVersion",
    "Payload flags: --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax -ffreestanding -fno-stack-protector -nostdlib",
    "Timebase: 10000000 Hz; one virtual tick per retired guest instruction; bounded explicit ticks while WFI waits",
    "ACLINT mapping: mtime=0x0200BFF8/8, mtimecmp=0x02004000/8, hart0 MTIP cause 7",
    ""
) + @($hashLines)
$manifest = $manifestLines -join "`r`n"
[IO.File]::WriteAllText((Join-Path $fixture "binary-manifest.txt"), $manifest + "`r`n", [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath (Join-Path $fixture "binary-manifest.txt")
