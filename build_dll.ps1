# Fix and Build DLL Project Only

Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  Building C++ DLL Project" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host ""

# Check if MSBuild is available
$msbuildPath = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuildPath)) {
    $msbuildPath = "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
}
if (-not (Test-Path $msbuildPath)) {
    $msbuildPath = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
}
if (-not (Test-Path $msbuildPath)) {
    Write-Error "MSBuild not found. Please build from Visual Studio instead."
    Write-Host ""
    Write-Host "In Visual Studio:" -ForegroundColor Yellow
    Write-Host "1. Right-click 'guideXOS Hypervisor DLL' project" -ForegroundColor White
    Write-Host "2. Select 'Rebuild'" -ForegroundColor White
    exit 1
}

Write-Host "Found MSBuild at: $msbuildPath" -ForegroundColor Green
Write-Host ""

# Build DLL project
Write-Host "Building DLL project..." -ForegroundColor Yellow
& $msbuildPath "guideXOS Hypervisor DLL.vcxproj" `
    /p:Configuration=Debug `
    /p:Platform=x64 `
    /p:PreprocessorDefinitions="GUIDEXOS_HYPERVISOR_EXPORTS;_DEBUG;_CONSOLE" `
    /t:Rebuild `
    /v:minimal

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "================================================" -ForegroundColor Green
    Write-Host "  ? DLL Built Successfully!" -ForegroundColor Green
    Write-Host "================================================" -ForegroundColor Green
    Write-Host ""
    
    # Check if DLL exists
    if (Test-Path "x64\Debug\guideXOS_Hypervisor.dll") {
        $dll = Get-Item "x64\Debug\guideXOS_Hypervisor.dll"
        Write-Host "DLL Location: $($dll.FullName)" -ForegroundColor White
        Write-Host "DLL Size: $($dll.Length) bytes" -ForegroundColor White
        Write-Host "Last Modified: $($dll.LastWriteTime)" -ForegroundColor White
        Write-Host ""
        
        # Check if it was copied to GUI
        $guiDll = "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll"
        if (Test-Path $guiDll) {
            Write-Host "? DLL copied to GUI output" -ForegroundColor Green
        } else {
            Write-Host "? DLL not copied to GUI - copying manually..." -ForegroundColor Yellow
            Copy-Item "x64\Debug\guideXOS_Hypervisor.dll" $guiDll -Force
            Write-Host "? DLL manually copied" -ForegroundColor Green
        }
    }
    
    Write-Host ""
    Write-Host "Next: Test the GUI" -ForegroundColor Cyan
    Write-Host "  cd 'guideXOS Hypervisor GUI'" -ForegroundColor White
    Write-Host "  dotnet run" -ForegroundColor White
} else {
    Write-Host ""
    Write-Host "================================================" -ForegroundColor Red
    Write-Host "  ? Build Failed" -ForegroundColor Red
    Write-Host "================================================" -ForegroundColor Red
    Write-Host ""
    Write-Host "Common issues:" -ForegroundColor Yellow
    Write-Host "1. GUIDEXOS_HYPERVISOR_EXPORTS not defined" -ForegroundColor White
    Write-Host "   - Should be defined in project properties" -ForegroundColor Gray
    Write-Host "2. Building wrong project (main EXE instead of DLL)" -ForegroundColor White
    Write-Host "   - Make sure to build DLL project specifically" -ForegroundColor Gray
    Write-Host "3. Core.lib not found" -ForegroundColor White
    Write-Host "   - Build Core library first" -ForegroundColor Gray
}

Write-Host ""
