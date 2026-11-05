param([string]$LlvmBin = "C:\Program Files\LLVM\bin")

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$fixture = $PSScriptRoot
$buildRoot = Join-Path $fixture ".build"
$clang = Join-Path $LlvmBin "clang.exe"
$objcopy = Join-Path $LlvmBin "llvm-objcopy.exe"
$readobj = Join-Path $LlvmBin "llvm-readobj.exe"
$objdump = Join-Path $LlvmBin "llvm-objdump.exe"
foreach ($tool in @($clang, $objcopy, $readobj, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool not found: $tool" }
}

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$elf = Join-Path $buildRoot "transition.elf"
$bin = Join-Path $buildRoot "transition.bin"
& $clang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax `
    -ffreestanding -fno-stack-protector -nostdlib `
    "-Wl,-T,$(Join-Path $fixture 'transition.ld')" `
    "-Wl,-Map,$(Join-Path $buildRoot 'transition.map')" `
    -o $elf (Join-Path $fixture "transition.S")
if ($LASTEXITCODE -ne 0) { throw "PMP transition guest ELF build failed" }
& $objcopy -S -O binary $elf $bin
if ($LASTEXITCODE -ne 0) { throw "PMP transition guest binary extraction failed" }
Copy-Item -LiteralPath $elf -Destination (Join-Path $fixture "transition.elf") -Force
Copy-Item -LiteralPath $bin -Destination (Join-Path $fixture "transition.bin") -Force
$elfInfo = & $readobj --file-headers --program-headers --sections --symbols $elf | Out-String
$disassembly = & $objdump --disassemble $elf | Out-String
[IO.File]::WriteAllText((Join-Path $fixture "transition-elf-info.txt"), $elfInfo,
    [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $fixture "transition-disassembly.txt"), $disassembly,
    [Text.UTF8Encoding]::new($false))
$version = (& $clang --version | Select-Object -First 1).Trim()
$manifest = @(
    "Payload compiler: $version",
    "Payload flags: --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax -ffreestanding -fno-stack-protector -nostdlib",
    "M-mode configures PMP, enters S-mode, grants and revokes one 8-byte read permission, and executes SFENCE.VMA after each PMP write.",
    ""
) -join "`r`n"
foreach ($name in @("transition.bin", "transition.elf")) {
    $file = Join-Path $fixture $name
    $manifest += "{0}  {1}  {2} bytes`r`n" -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash,
        $name, (Get-Item -LiteralPath $file).Length
}
[IO.File]::WriteAllText((Join-Path $fixture "binary-manifest.txt"), $manifest,
    [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath (Join-Path $fixture "binary-manifest.txt")
