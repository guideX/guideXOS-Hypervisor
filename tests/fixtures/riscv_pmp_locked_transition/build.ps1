param(
    [string]$LlvmBin = "C:\Program Files\LLVM\bin",
    [string]$BuildRoot = "E:\CodexArtifacts\guideXOS_Hypervisor\phase-riscv17-build\pmp-locked"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$fixture = $PSScriptRoot
$clang = Join-Path $LlvmBin "clang.exe"
$objcopy = Join-Path $LlvmBin "llvm-objcopy.exe"
foreach ($tool in @($clang, $objcopy)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool not found: $tool" }
}
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$elf = Join-Path $BuildRoot "locked_transition.elf"
$bin = Join-Path $BuildRoot "locked_transition.bin"
& $clang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax `
    -ffreestanding -fno-stack-protector -nostdlib `
    "-Wl,-T,$(Join-Path $fixture 'locked_transition.ld')" `
    "-Wl,-Map,$(Join-Path $BuildRoot 'locked_transition.map')" `
    -o $elf (Join-Path $fixture "locked_transition.S")
if ($LASTEXITCODE -ne 0) { throw "Locked PMP transition guest ELF build failed" }
& $objcopy -S -O binary $elf $bin
if ($LASTEXITCODE -ne 0) { throw "Locked PMP transition guest binary extraction failed" }

Copy-Item -LiteralPath $bin -Destination (Join-Path $fixture "locked_transition.bin") -Force
Copy-Item -LiteralPath $elf -Destination (Join-Path $fixture "locked_transition.elf") -Force
$compiler = (& $clang --version | Select-Object -First 1).Trim()
$lines = @(
    "Guest compiler: $compiler",
    "Guest flags: --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax -ffreestanding -fno-stack-protector -nostdlib",
    "Guest uses M-mode pmpaddr/pmpcfg CSR instructions to test locked NAPOT and locked TOR predecessor writes.",
    "A production CPU reset clears the locks; the preserved guest phase marker then reconfigures and changes the same region.",
    ""
)
foreach ($name in @("locked_transition.bin", "locked_transition.elf")) {
    $file = Join-Path $fixture $name
    $lines += "{0}  {1}  {2} bytes" -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash,
        $name, (Get-Item -LiteralPath $file).Length
}
[IO.File]::WriteAllText((Join-Path $fixture "binary-manifest.txt"),
    ($lines -join "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath (Join-Path $fixture "binary-manifest.txt")
