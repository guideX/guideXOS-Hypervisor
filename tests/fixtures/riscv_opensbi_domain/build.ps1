param(
    [string]$LlvmBin = "C:\Program Files\LLVM\bin",
    [string]$Dtc = "C:\msys64\usr\bin\dtc.exe"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$fixture = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $fixture "..\..\..")).Path
$buildRoot = Join-Path $fixture ".build"
$llvmClang = Join-Path $LlvmBin "clang.exe"
$readobj = Join-Path $LlvmBin "llvm-readobj.exe"
$objdump = Join-Path $LlvmBin "llvm-objdump.exe"
$opensbiFixture = Join-Path $repo "tests\fixtures\riscv_opensbi_fw_jump"
$firmwareBin = Join-Path $opensbiFixture "fw_jump.bin"
$firmwareElf = Join-Path $opensbiFixture "fw_jump.elf"
$expectedFirmwareHash = "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3"
$expectedFirmwareElfHash = "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0"

foreach ($tool in @($llvmClang, $readobj, $objdump, $Dtc, $firmwareBin, $firmwareElf)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool or pinned input not found: $tool" }
}
if ((Get-FileHash -LiteralPath $firmwareBin -Algorithm SHA256).Hash -ne $expectedFirmwareHash) {
    throw "The accepted OpenSBI fw_jump.bin hash does not match."
}
if ((Get-FileHash -LiteralPath $firmwareElf -Algorithm SHA256).Hash -ne $expectedFirmwareElfHash) {
    throw "The accepted OpenSBI fw_jump.elf hash does not match."
}

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$dtbBuild = Join-Path $buildRoot "platform.dtb"
$elfBuild = Join-Path $buildRoot "supervisor.elf"
& $Dtc -I dts -O dtb -o $dtbBuild (Join-Path $fixture "platform.dts")
if ($LASTEXITCODE -ne 0) { throw "RISCV-16 domain FDT build failed" }
& $llvmClang --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax `
    -ffreestanding -fno-stack-protector -nostdlib `
    "-Wl,-T,$(Join-Path $fixture 'supervisor.ld')" `
    "-Wl,-Map,$(Join-Path $buildRoot 'supervisor.map')" `
    -o $elfBuild (Join-Path $fixture "supervisor.S")
if ($LASTEXITCODE -ne 0) { throw "RISCV-16 supervisor payload build failed" }

Copy-Item -LiteralPath $dtbBuild -Destination (Join-Path $fixture "platform.dtb") -Force
Copy-Item -LiteralPath $elfBuild -Destination (Join-Path $fixture "supervisor.elf") -Force
$elfInfo = & $readobj --file-headers --program-headers --sections --symbols $elfBuild | Out-String
$disassembly = & $objdump --disassemble $elfBuild | Out-String
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-elf-info.txt"), $elfInfo,
    [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $fixture "supervisor-disassembly.txt"), $disassembly,
    [Text.UTF8Encoding]::new($false))

$compilerVersion = (& $llvmClang --version | Select-Object -First 1).Trim()
$hashLines = foreach ($name in @("platform.dtb", "supervisor.elf")) {
    $file = Join-Path $fixture $name
    "{0}  {1}  {2} bytes" -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash,
        $name, (Get-Item -LiteralPath $file).Length
}
$manifestLines = @(
    "OpenSBI source commit: 74434f255873d74e56cc50aa762d1caf24c099f8 (unchanged)",
    "OpenSBI fw_jump.bin SHA-256: $expectedFirmwareHash",
    "OpenSBI fw_jump.elf SHA-256: $expectedFirmwareElfHash",
    "Payload compiler: $compilerVersion",
    "Payload flags: --target=riscv64-unknown-elf -march=rv64ima_zicsr -mabi=lp64 -mno-relax -ffreestanding -fno-stack-protector -nostdlib",
    "FDT compiler: $(& $Dtc --version)",
    "Payload entry=0x80200000; page tables at 0x83c00000; independently assembled S-mode ELF",
    "The build reads the pinned firmware and does not rebuild or alter OpenSBI.",
    ""
) + @($hashLines)
$manifest = $manifestLines -join "`r`n"
[IO.File]::WriteAllText((Join-Path $fixture "binary-manifest.txt"), $manifest + "`r`n",
    [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath (Join-Path $fixture "binary-manifest.txt")
