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
if ($LASTEXITCODE -ne 0) { throw "PLIC/UART supervisor payload build failed" }
& $objcopy -S -O binary $elfBuild $binBuild
if ($LASTEXITCODE -ne 0) { throw "PLIC/UART supervisor binary extraction failed" }
& $dtc -I dts -O dtb -o $dtbBuild (Join-Path $fixture "platform.dts")
if ($LASTEXITCODE -ne 0) { throw "PLIC/UART FDT compilation failed" }

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
    "PLIC map: base=0x0C000000 size=0x00400000; sources=96; M/S contexts=0/1",
    "UART0: 0x10000000, non-FIFO single-byte RX, PLIC source 10",
    "UART1: 0x1000A000, independent non-FIFO single-byte RX, PLIC source 12",
    "Guest Sv39: identity-mapped low 1 GiB for both UARTs/PLIC and 0x80000000 RAM leaf",
    "Guest scenario log: 5 simultaneous-pending pairs; S-mode claims, reads, completions and SRETs",
    ""
) + @($hashLines)
[IO.File]::WriteAllText((Join-Path $fixture "binary-manifest.txt"),
    ($manifestLines -join "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath (Join-Path $fixture "binary-manifest.txt")
