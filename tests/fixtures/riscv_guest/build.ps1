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
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required LLVM tool not found: $tool"
    }
}

$clangId = (& $clang --version | Select-Object -First 1).Trim()
$linkerId = (& $linker --version | Select-Object -First 1).Trim()
$objcopyId = (& $objcopy --version | Select-Object -First 1).Trim()

Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    & $clang --target=riscv64-unknown-elf -march=rv64i_zicsr -mabi=lp64 `
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

    $hash = (Get-FileHash -LiteralPath guest.bin -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath guest.bin).Length
    @(
        "Assembler: $clangId (LLVM integrated assembler)"
        "Linker: $linkerId"
        "Objcopy: $objcopyId"
        'Target: riscv64-unknown-elf'
        'ISA: rv64i_zicsr'
        'ABI: lp64 (soft-float; no floating-point ABI)'
        'Relaxation: disabled'
        'Compressed instructions: disabled (.option norvc)'
        'Assembly command: clang.exe --target=riscv64-unknown-elf -march=rv64i_zicsr -mabi=lp64 -mcmodel=medany -fno-pic -mno-relax -c guest.S -o .build/guest.o'
        'Link command: ld.lld.exe --no-relax --build-id=none -T guest.ld --Map=.build/guest.map -o .build/guest.elf .build/guest.o'
        'Binary command: llvm-objcopy.exe -O binary .build/guest.elf guest.bin'
        'Entry symbol: _start'
        'Load address: 0x80000000'
        "Binary size: $size"
        "SHA-256: $hash"
    ) | Set-Content -Encoding ascii binary-manifest.txt
    Write-Host "Built guest.bin ($size bytes), SHA-256 $hash"
}
finally {
    Pop-Location
}
