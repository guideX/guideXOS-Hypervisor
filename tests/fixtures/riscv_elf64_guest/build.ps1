param(
    [string]$LlvmBin = (Join-Path $env:ProgramFiles 'LLVM\bin')
)

$ErrorActionPreference = 'Stop'
$clang = Join-Path $LlvmBin 'clang.exe'
$linker = Join-Path $LlvmBin 'ld.lld.exe'
$readobj = Join-Path $LlvmBin 'llvm-readobj.exe'
$objdump = Join-Path $LlvmBin 'llvm-objdump.exe'
$objcopy = Join-Path $LlvmBin 'llvm-objcopy.exe'
$buildDir = Join-Path $PSScriptRoot '.build'
$objectFile = Join-Path $buildDir 'guest.o'
$builtElf = Join-Path $buildDir 'guest.elf'
$mapFile = Join-Path $buildDir 'guest.map'
$acceptedElf = Join-Path $PSScriptRoot 'guest.elf'

foreach ($tool in @($clang, $linker, $readobj, $objdump, $objcopy)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required LLVM tool not found: $tool"
    }
}

$clangId = (& $clang --version | Select-Object -First 1).Trim()
$linkerId = (& $linker --version | Select-Object -First 1).Trim()
$objcopyId = (& $objcopy --version | Select-Object -First 1).Trim()

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
Push-Location $PSScriptRoot
try {
    & $clang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mcmodel=medany -fno-pic -mno-relax -c guest.S -o $objectFile
    if ($LASTEXITCODE -ne 0) { throw "Clang integrated assembler failed with exit code $LASTEXITCODE" }

    & $linker --no-relax --build-id=none -T guest.ld "--Map=$mapFile" -o $builtElf $objectFile
    if ($LASTEXITCODE -ne 0) { throw "LLD link failed with exit code $LASTEXITCODE" }

    & $objcopy -O binary $builtElf (Join-Path $buildDir 'guest.bin')
    if ($LASTEXITCODE -ne 0) { throw "LLVM objcopy failed with exit code $LASTEXITCODE" }

    Copy-Item -LiteralPath $builtElf -Destination $acceptedElf -Force
    & $readobj --file-headers --program-headers --sections --arch-specific $acceptedElf | Set-Content -Encoding ascii (Join-Path $PSScriptRoot 'guest.elf-info.txt')
    if ($LASTEXITCODE -ne 0) { throw "LLVM readobj failed with exit code $LASTEXITCODE" }

    & $objdump -d --no-show-raw-insn $acceptedElf | Set-Content -Encoding ascii (Join-Path $PSScriptRoot 'guest.disassembly.txt')
    if ($LASTEXITCODE -ne 0) { throw "LLVM objdump failed with exit code $LASTEXITCODE" }

    $sha256 = (Get-FileHash -LiteralPath $acceptedElf -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $acceptedElf).Length
    @(
        "Clang: $clangId (LLVM integrated assembler)"
        "Linker: $linkerId"
        "Objcopy: $objcopyId"
        'Target: riscv64-unknown-elf'
        'ISA: rv64ima_zicsr (RV64IMA plus Zicsr)'
        'ABI: lp64 soft-float'
        'ELF: ELF64 little-endian EM_RISCV ET_EXEC, statically linked'
        'Compressed instructions: disabled by .option norvc'
        'Linker relaxation: disabled'
        'Dynamic linking: disabled; no PT_INTERP or PT_DYNAMIC'
        'Compile: clang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mcmodel=medany -fno-pic -mno-relax -c guest.S'
        'Link: ld.lld --no-relax --build-id=none -T guest.ld guest.o'
        'Entry: _start at 0x80200000'
        'PT_LOAD layout: RX 0x80200000, R 0x80400000, RW 0x80600000'
        "ELF size: $size bytes"
        "SHA-256: $sha256"
    ) | Set-Content -Encoding ascii (Join-Path $PSScriptRoot 'binary-manifest.txt')

    Write-Host "Built static RV64IMA ELF64 guest ($size bytes), SHA-256 $sha256"
}
finally {
    Pop-Location
}
