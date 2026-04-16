# Exclude Source Files from EXE Project
# This script marks all src\*.cpp files as excluded from build in the main EXE project
# Run this from the solution root directory

$projectFile = "guideXOS Hypervisor.vcxproj"

if (-not (Test-Path $projectFile)) {
    Write-Error "Could not find $projectFile. Make sure you're in the solution root directory."
    exit 1
}

Write-Host "Reading project file..." -ForegroundColor Cyan
$xml = [xml](Get-Content $projectFile)

# Define namespace
$ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$ns.AddNamespace("ns", "http://schemas.microsoft.com/developer/msbuild/2003")

# Find all ClCompile elements that start with 'src\'
$srcFiles = $xml.SelectNodes("//ns:ClCompile[starts-with(@Include, 'src\')]", $ns)

Write-Host "Found $($srcFiles.Count) source files in src\ directory" -ForegroundColor Yellow

$excluded = 0
foreach ($file in $srcFiles) {
    $fileName = $file.GetAttribute("Include")
    
    # Skip if already excluded
    if ($file.GetAttribute("ExcludedFromBuild") -eq "true") {
        Write-Host "  - $fileName (already excluded)" -ForegroundColor Gray
        continue
    }
    
    # Exclude from build
    $file.SetAttribute("ExcludedFromBuild", "true")
    Write-Host "  ? Excluded: $fileName" -ForegroundColor Green
    $excluded++
}

if ($excluded -gt 0) {
    Write-Host "`nSaving changes to project file..." -ForegroundColor Cyan
    $xml.Save((Resolve-Path $projectFile).Path)
    Write-Host "? Successfully excluded $excluded files from build" -ForegroundColor Green
} else {
    Write-Host "`nNo changes needed - all files already excluded" -ForegroundColor Yellow
}

Write-Host "`nNext steps:" -ForegroundColor Cyan
Write-Host "1. Add project reference to Core.lib in project properties" -ForegroundColor White
Write-Host "2. Build the solution" -ForegroundColor White
