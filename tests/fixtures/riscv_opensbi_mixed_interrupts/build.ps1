param(
    [string]$MsysRoot = "C:\msys64",
    [string]$LlvmBin = "C:\Program Files\LLVM\bin",
    [string]$BuildRoot = (Join-Path $PSScriptRoot ".build")
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$fixture = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $fixture "..\..\..\")).Path
$llvmClang = Join-Path $LlvmBin "clang.exe"
$objcopy = Join-Path $LlvmBin "llvm-objcopy.exe"
$readobj = Join-Path $LlvmBin "llvm-readobj.exe"
$objdump = Join-Path $LlvmBin "llvm-objdump.exe"
$dtc = Join-Path $MsysRoot "usr\bin\dtc.exe"
$firmware = Join-Path $repo "tests\fixtures\riscv_opensbi_fw_jump\fw_jump.bin"
$firmwareElf = Join-Path $repo "tests\fixtures\riscv_opensbi_fw_jump\fw_jump.elf"
$firmwareSha = "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3"
$firmwareElfSha = "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0"

foreach ($tool in @($llvmClang, $objcopy, $readobj, $objdump, $dtc)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool not found: $tool" }
}
if ((Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash -ne $firmwareSha -or
    (Get-FileHash -LiteralPath $firmwareElf -Algorithm SHA256).Hash -ne $firmwareElfSha) {
    throw "The accepted RISCV-9 OpenSBI v1.8.1 firmware hashes do not match."
}

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$elfBuild = Join-Path $BuildRoot "supervisor.elf"
$binBuild = Join-Path $BuildRoot "supervisor.bin"
$dtbBuild = Join-Path $BuildRoot "platform.dtb"
$ldScript = Join-Path $fixture "supervisor.ld"
$mapFile = Join-Path $BuildRoot "supervisor.map"
$clangArgs = @(
    "--target=riscv64-unknown-elf", "-march=rv64ima_zicsr", "-mabi=lp64", "-mno-relax",
    "-ffreestanding", "-fno-stack-protector", "-nostdlib",
    "-Wl,-T,$ldScript", "-Wl,-Map,$mapFile", "-o", $elfBuild,
    (Join-Path $fixture "supervisor.S")
)
& $llvmClang @clangArgs
if ($LASTEXITCODE -ne 0) { throw "Mixed-interrupt supervisor payload build failed" }
& $objcopy -S -O binary $elfBuild $binBuild
if ($LASTEXITCODE -ne 0) { throw "Supervisor binary extraction failed" }
& $dtc -I dts -O dtb -o $dtbBuild (Join-Path $fixture "platform.dts")
if ($LASTEXITCODE -ne 0) { throw "Mixed-interrupt FDT compilation failed" }

Copy-Item -LiteralPath $elfBuild -Destination (Join-Path $fixture "supervisor.elf") -Force
Copy-Item -LiteralPath $binBuild -Destination (Join-Path $fixture "supervisor.bin") -Force
Copy-Item -LiteralPath $dtbBuild -Destination (Join-Path $fixture "platform.dtb") -Force
$elfInfo = & $readobj --file-headers --program-headers --sections --symbols $elfBuild | Out-String
$disassembly = & $objdump --disassemble $elfBuild | Out-String
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-elf-info.txt"), $elfInfo, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-disassembly.txt"), $disassembly, [Text.UTF8Encoding]::new($false))
$compilerVersion = (& $llvmClang --version | Select-Object -First 1).Trim()
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
    "Reused OpenSBI fw_jump.elf SHA-256: $firmwareElfSha",
    "Payload compiler: $compilerVersion",
    "DTC: $dtcVersion",
    "Payload flags: --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax -ffreestanding -fno-stack-protector -nostdlib",
    "Timer: ACLINT mtime=0x0200BFF8; mtimecmp=0x02004000; guest uses genuine OpenSBI TIME",
    "PLIC: base=0x0C000000 size=0x00400000; 96 sources; M/S contexts=0/1",
    "UART0: 0x10000000, non-FIFO receive, PLIC source 10; UART1: 0x1000A000, source 12",
    "Guest: independently assembled RV64IMA_Zicsr; Sv39; authentic unmodified OpenSBI v1.8.1",
    "Scenarios: simultaneous SEIP/STIP, STIE mask, SEIE mask, masked-WFI wake, PLIC threshold, UART IER, three-way PLIC arbitration",
    "Build output directory: $BuildRoot",
    ""
) + @($hashLines)
[IO.File]::WriteAllText((Join-Path $fixture "binary-manifest.txt"),
    ($manifestLines -join "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath (Join-Path $fixture "binary-manifest.txt")
