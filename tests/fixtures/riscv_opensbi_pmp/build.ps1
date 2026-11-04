param(
    [string]$LlvmBin = "C:\Program Files\LLVM\bin"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$fixture = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $fixture "..\..\..")).Path
$buildRoot = Join-Path $fixture ".build"
$llvmClang = Join-Path $LlvmBin "clang.exe"
$objcopy = Join-Path $LlvmBin "llvm-objcopy.exe"
$readobj = Join-Path $LlvmBin "llvm-readobj.exe"
$objdump = Join-Path $LlvmBin "llvm-objdump.exe"
$openSbiFixture = Join-Path $repo "tests\fixtures\riscv_opensbi_fw_jump"
$firmwareBin = Join-Path $openSbiFixture "fw_jump.bin"
$firmwareElf = Join-Path $openSbiFixture "fw_jump.elf"
$platformFdt = Join-Path $openSbiFixture "platform.dtb"
$expectedFirmwareHash = "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3"
$expectedFirmwareElfHash = "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0"
$expectedFdtHash = "B2CBFE6E2A6C4D21616BEC6EEF216A9A42F7068B9853505C700751FA7C92E093"

foreach ($tool in @($llvmClang, $objcopy, $readobj, $objdump, $firmwareBin, $firmwareElf, $platformFdt)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool or pinned input not found: $tool" }
}
if ((Get-FileHash -LiteralPath $firmwareBin -Algorithm SHA256).Hash -ne $expectedFirmwareHash) {
    throw "The accepted OpenSBI fw_jump.bin hash does not match."
}
if ((Get-FileHash -LiteralPath $firmwareElf -Algorithm SHA256).Hash -ne $expectedFirmwareElfHash) {
    throw "The accepted OpenSBI fw_jump.elf hash does not match."
}
if ((Get-FileHash -LiteralPath $platformFdt -Algorithm SHA256).Hash -ne $expectedFdtHash) {
    throw "The accepted RISCV-9 platform FDT hash does not match."
}

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$elfBuild = Join-Path $buildRoot "supervisor.elf"
$binBuild = Join-Path $buildRoot "supervisor.bin"
& $llvmClang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax `
    -ffreestanding -fno-stack-protector -nostdlib `
    "-Wl,-T,$(Join-Path $fixture 'supervisor.ld')" `
    "-Wl,-Map,$(Join-Path $buildRoot 'supervisor.map')" `
    -o $elfBuild (Join-Path $fixture "supervisor.S")
if ($LASTEXITCODE -ne 0) { throw "PMP supervisor payload build failed" }
& $objcopy -S -O binary $elfBuild $binBuild
if ($LASTEXITCODE -ne 0) { throw "PMP supervisor binary extraction failed" }

Copy-Item -LiteralPath $elfBuild -Destination (Join-Path $fixture "supervisor.elf") -Force
Copy-Item -LiteralPath $binBuild -Destination (Join-Path $fixture "supervisor.bin") -Force
$elfInfo = & $readobj --file-headers --program-headers --sections --symbols $elfBuild | Out-String
$disassembly = & $objdump --disassemble $elfBuild | Out-String
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-elf-info.txt"), $elfInfo, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-disassembly.txt"), $disassembly, [Text.UTF8Encoding]::new($false))

$compilerVersion = (& $llvmClang --version | Select-Object -First 1).Trim()
$hashLines = foreach ($name in @("supervisor.bin", "supervisor.elf")) {
    $file = Join-Path $fixture $name
    "{0}  {1}  {2} bytes" -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash,
        $name, (Get-Item -LiteralPath $file).Length
}
$manifestLines = @(
    "OpenSBI source commit: 74434f255873d74e56cc50aa762d1caf24c099f8 (unchanged)",
    "OpenSBI fw_jump.bin SHA-256: $expectedFirmwareHash",
    "OpenSBI fw_jump.elf SHA-256: $expectedFirmwareElfHash",
    "Platform FDT SHA-256: $expectedFdtHash",
    "Payload compiler: $compilerVersion",
    "Payload flags: --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax -ffreestanding -fno-stack-protector -nostdlib",
    "Linker script: supervisor.ld; entry=0x80200000; independent freestanding ELF",
    "The build reads the firmware/FDT inputs and does not rebuild them.",
    ""
) + @($hashLines)
$manifest = $manifestLines -join "`r`n"
[IO.File]::WriteAllText((Join-Path $fixture "binary-manifest.txt"), $manifest + "`r`n", [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath (Join-Path $fixture "binary-manifest.txt")
