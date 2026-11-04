param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$LlvmBin,
    [switch]$Overwrite
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $LlvmBin) {
    $LlvmBin = Join-Path $repoRoot 'build\mips64-12-toolchain\portable\bin'
}
$clang = Join-Path $LlvmBin 'clang.exe'
$linker = Join-Path $LlvmBin 'ld.lld.exe'
$out = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $clang) -or -not (Test-Path -LiteralPath $linker)) {
    throw "Clang and LLD were not found in $LlvmBin"
}
if (-not (Test-Path -LiteralPath $out)) {
    New-Item -ItemType Directory -Path $out | Out-Null
}
$objectDirectory = Join-Path $out '.objects'
if (-not (Test-Path -LiteralPath $objectDirectory)) {
    New-Item -ItemType Directory -Path $objectDirectory | Out-Null
}

foreach ($mode in @(
    @{ Name = 'le'; Triple = 'mips64el-unknown-elf'; Endian = '-EL'; Emulation = 'elf64ltsmip' },
    @{ Name = 'be'; Triple = 'mips64-unknown-elf'; Endian = '-EB'; Emulation = 'elf64btsmip' }
)) {
    $elf = Join-Path $out "guidexos_mips64_$($mode.Name).elf"
    $obj = Join-Path $objectDirectory "guidexos_mips64_$($mode.Name).o"
    if ((Test-Path -LiteralPath $elf) -and -not $Overwrite) {
        throw "Refusing to overwrite existing artifact $elf; pass -Overwrite to replace generated fixtures."
    }

    $targetArgs = @('--target=' + $mode.Triple, '-march=mips64r2', '-mabi=64', '-G0',
                    '-mno-abicalls', '-fno-pic', $mode.Endian)
    & $clang @targetArgs -c (Join-Path $PSScriptRoot 'handoff.S') -o $obj
    if ($LASTEXITCODE -ne 0) { throw "Clang assembly failed for $($mode.Name)" }

    & $linker -m $mode.Emulation -T (Join-Path $PSScriptRoot 'linker.ld') `
        --static --build-id=none --no-relax $obj -o $elf
    if ($LASTEXITCODE -ne 0) { throw "Clang/LLD link failed for $($mode.Name)" }
}

& (Join-Path $LlvmBin 'llvm-readobj.exe') --file-headers --program-headers `
    (Join-Path $out 'guidexos_mips64_le.elf') `
    (Join-Path $out 'guidexos_mips64_be.elf')
