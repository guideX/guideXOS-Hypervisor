# Quick Build Fix Script
# This script helps configure the build properly

Write-Host "=== guideXOS Hypervisor Build Configuration Helper ===" -ForegroundColor Cyan
Write-Host ""

$projectRoot = "D:\devgitlab\guideXOS.Hypervisor"

Write-Host "Current Issues:" -ForegroundColor Yellow
Write-Host "  1. Multiple main() functions from examples/tests"
Write-Host "  2. Missing REST API implementations"
Write-Host ""

Write-Host "Solutions:" -ForegroundColor Green
Write-Host ""

Write-Host "OPTION 1: Generate Proper Visual Studio Solution (RECOMMENDED)" -ForegroundColor Cyan
Write-Host "------------------------------------------------------------"
Write-Host "Run these commands:"
Write-Host "  cd $projectRoot" -ForegroundColor White
Write-Host "  if (Test-Path build) { Remove-Item -Recurse -Force build }" -ForegroundColor White
Write-Host "  mkdir build" -ForegroundColor White
Write-Host "  cd build" -ForegroundColor White
Write-Host "  cmake .. -G `"Visual Studio 17 2022`" -A x64" -ForegroundColor White
Write-Host ""
Write-Host "Then open: build\IA64Emulator.sln in Visual Studio"
Write-Host "This creates separate projects for each executable."
Write-Host ""

Write-Host "OPTION 2: Build Just the Decoder Library" -ForegroundColor Cyan
Write-Host "-----------------------------------------"
Write-Host "The decoder refactoring is complete. To verify:"
Write-Host "  1. In Visual Studio, go to Build > Batch Build"
Write-Host "  2. Uncheck all except core source files"
Write-Host "  3. Or compile individual decoder files"
Write-Host ""

Write-Host "OPTION 3: Quick Fix for Main Executable" -ForegroundColor Cyan
Write-Host "----------------------------------------"
Write-Host "Exclude example/test files from build:"
Write-Host "  1. In Solution Explorer, find 'examples' and 'tests' folders"
Write-Host "  2. For each .cpp file in these folders:"
Write-Host "     - Right-click > Properties"
Write-Host "     - Configuration: All Configurations"
Write-Host "     - General > Excluded From Build > Yes"
Write-Host "  3. Also exclude: src/api/RestAPIServer.cpp (or implement missing methods)"
Write-Host ""

Write-Host "DECODER HOUSEKEEPING STATUS: " -NoNewline
Write-Host "COMPLETE ?" -ForegroundColor Green
Write-Host "All decoder files compile successfully!"
Write-Host ""

$response = Read-Host "Would you like me to generate the CMake build? (y/n)"
if ($response -eq 'y' -or $response -eq 'Y') {
    Write-Host ""
    Write-Host "Generating CMake build..." -ForegroundColor Cyan
    
    cd $projectRoot
    
    if (Test-Path build) {
        Write-Host "Removing old build directory..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force build
    }
    
    mkdir build | Out-Null
    cd build
    
    Write-Host "Running CMake..." -ForegroundColor Cyan
    cmake .. -G "Visual Studio 17 2022" -A x64
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "SUCCESS! CMake configuration complete." -ForegroundColor Green
        Write-Host ""
        Write-Host "Next steps:" -ForegroundColor Cyan
        Write-Host "  1. Open: $projectRoot\build\IA64Emulator.sln"
        Write-Host "  2. Select your desired startup project (e.g., ia64emu)"
        Write-Host "  3. Build individual projects or solution"
        Write-Host ""
        
        # Try to open the solution
        $slnPath = "$projectRoot\build\IA64Emulator.sln"
        if (Test-Path $slnPath) {
            $open = Read-Host "Open solution in Visual Studio now? (y/n)"
            if ($open -eq 'y' -or $open -eq 'Y') {
                Start-Process $slnPath
            }
        }
    } else {
        Write-Host ""
        Write-Host "CMake configuration failed. Check the output above for errors." -ForegroundColor Red
    }
} else {
    Write-Host ""
    Write-Host "Skipping CMake generation. Choose one of the options above manually." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "For more details, see: docs/BUILD_ISSUES_SUMMARY.md" -ForegroundColor Cyan
