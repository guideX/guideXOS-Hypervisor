# Fix Core Library Project
# This script removes ExcludedFromBuild attributes from all source files

$projectFile = "guideXOS Hypervisor Core.vcxproj"

if (-not (Test-Path $projectFile)) {
    Write-Error "Core project file not found: $projectFile"
    Write-Host "Make sure you're running this from the solution root directory" -ForegroundColor Red
    exit 1
}

Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  Fixing Core Library Project" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host ""

# Load project file
Write-Host "Loading project file..." -ForegroundColor Yellow
$xml = [xml](Get-Content $projectFile -Encoding UTF8)

# Create namespace manager
$ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$ns.AddNamespace("ns", "http://schemas.microsoft.com/developer/msbuild/2003")

# Find all ClCompile elements
$allCompiles = $xml.SelectNodes("//ns:ClCompile", $ns)
Write-Host "Total compile items found: $($allCompiles.Count)" -ForegroundColor White

# Find excluded files
$excludedCount = 0
$fixedCount = 0

foreach ($compile in $allCompiles) {
    $fileName = $compile.GetAttribute("Include")
    $hasExcludedAttr = $compile.HasAttribute("ExcludedFromBuild")
    $hasExcludedChild = $compile.SelectNodes("ns:ExcludedFromBuild", $ns).Count -gt 0
    
    if ($hasExcludedAttr -or $hasExcludedChild) {
        $excludedCount++
        
        # Remove attribute
        if ($hasExcludedAttr) {
            $compile.RemoveAttribute("ExcludedFromBuild")
        }
        
        # Remove child elements
        $childNodes = $compile.SelectNodes("ns:ExcludedFromBuild", $ns)
        foreach ($child in $childNodes) {
            [void]$compile.RemoveChild($child)
        }
        
        Write-Host "  ? Included: $fileName" -ForegroundColor Green
        $fixedCount++
    }
}

Write-Host ""
Write-Host "Summary:" -ForegroundColor Cyan
Write-Host "  Total files: $($allCompiles.Count)" -ForegroundColor White
Write-Host "  Previously excluded: $excludedCount" -ForegroundColor Yellow
Write-Host "  Now included: $fixedCount" -ForegroundColor Green

if ($fixedCount -gt 0) {
    # Save the file
    Write-Host ""
    Write-Host "Saving changes..." -ForegroundColor Yellow
    $xml.Save((Resolve-Path $projectFile).Path)
    
    Write-Host ""
    Write-Host "================================================" -ForegroundColor Green
    Write-Host "  ? Core project fixed successfully!" -ForegroundColor Green
    Write-Host "================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Cyan
    Write-Host "  1. Open Visual Studio" -ForegroundColor White
    Write-Host "  2. Right-click 'guideXOS Hypervisor Core' project" -ForegroundColor White
    Write-Host "  3. Select 'Clean'" -ForegroundColor White
    Write-Host "  4. Select 'Rebuild'" -ForegroundColor White
    Write-Host "  5. Verify Core.lib is now 10-20 MB (was ~400 KB)" -ForegroundColor White
} else {
    Write-Host ""
    Write-Host "No changes needed - all files already included!" -ForegroundColor Yellow
}

Write-Host ""
