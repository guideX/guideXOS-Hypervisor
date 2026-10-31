param(
    [string]$LlvmBin = (Join-Path $env:ProgramFiles 'LLVM\bin')
)

$ErrorActionPreference = 'Stop'
$clang = Join-Path $LlvmBin 'clang.exe'
$linker = Join-Path $LlvmBin 'ld.lld.exe'
$objcopy = Join-Path $LlvmBin 'llvm-objcopy.exe'
$objdump = Join-Path $LlvmBin 'llvm-objdump.exe'
$readobj = Join-Path $LlvmBin 'llvm-readobj.exe'
$buildDir = Join-Path $PSScriptRoot '.build'
$source = Join-Path $PSScriptRoot 'guest.S'
$linkerScript = Join-Path $PSScriptRoot 'guest.ld'
$objectFile = Join-Path $buildDir 'guest.o'
$elfFile = Join-Path $buildDir 'guest.elf'
$mapFile = Join-Path $buildDir 'guest.map'

foreach ($tool in @($clang, $linker, $objcopy, $objdump, $readobj)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required LLVM tool not found: $tool" }
}

$clangId = (& $clang --version | Select-Object -First 1).Trim()
$linkerId = (& $linker --version | Select-Object -First 1).Trim()
$llvmVersion = 'unknown'
if ($clangId -match 'version\s+([0-9.]+)') { $llvmVersion = $Matches[1] }
$objcopyId = "llvm-objcopy (LLVM $llvmVersion; GNU compatible)"
$objdumpId = "llvm-objdump (LLVM $llvmVersion)"
$readobjId = "llvm-readobj (LLVM $llvmVersion)"

Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    & $clang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 `
        -mcmodel=medany -fno-pic -mno-relax -c $source -o $objectFile
    if ($LASTEXITCODE -ne 0) { throw "Clang assembler failed with exit code $LASTEXITCODE" }

    & $linker --no-relax --build-id=none -T $linkerScript "--Map=$mapFile" `
        -o $elfFile $objectFile
    if ($LASTEXITCODE -ne 0) { throw "LLD link failed with exit code $LASTEXITCODE" }

    & $objcopy -O binary $elfFile (Join-Path $PSScriptRoot 'guest.bin')
    if ($LASTEXITCODE -ne 0) { throw "LLVM objcopy failed with exit code $LASTEXITCODE" }

    & $objdump -d $elfFile | Set-Content -Encoding ascii guest.disassembly.txt
    if ($LASTEXITCODE -ne 0) { throw "LLVM objdump failed with exit code $LASTEXITCODE" }
    & $readobj --file-headers --arch-specific $elfFile |
        Set-Content -Encoding ascii guest.elf-info.txt
    if ($LASTEXITCODE -ne 0) { throw "LLVM readobj failed with exit code $LASTEXITCODE" }

    $inventory = (& $objdump -d $elfFile | Select-String -Pattern '^\s*[0-9a-f]+:')
    $inventory | ForEach-Object { $_.Line.Trim() } |
        Set-Content -Encoding ascii instruction-inventory.txt
    $hash = (Get-FileHash -LiteralPath guest.bin -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath guest.bin).Length
    @(
        "Assembler: $clangId (LLVM integrated RISC-V assembler)"
        "Linker: $linkerId"
        "Objcopy: $objcopyId"
        "Objdump: $objdumpId"
        "Readobj: $readobjId"
        'Target: riscv64-unknown-elf'
        'ISA: rv64ima_zicsr'
        'ABI: lp64 (soft-float; no floating-point ABI)'
        'Relaxation: disabled at compile and link time'
        'Compressed instructions: disabled with .option norvc'
        'F/D/V extensions: not enabled'
        'Load address: 0x80000000'
        'Entry point: _start at 0x80000000'
        "Binary size: $size bytes"
        "SHA-256: $hash"
        'Build command: clang.exe --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mcmodel=medany -fno-pic -mno-relax -c guest.S -o .build/guest.o'
        'Link command: ld.lld.exe --no-relax --build-id=none -T guest.ld --Map=.build/guest.map -o .build/guest.elf .build/guest.o'
        'Binary command: llvm-objcopy.exe -O binary .build/guest.elf guest.bin'
    ) | Set-Content -Encoding ascii binary-manifest.txt
    Write-Host "Built $size bytes; SHA-256 $hash"
}
finally {
    Pop-Location
}
