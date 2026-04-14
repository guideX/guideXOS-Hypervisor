# PowerShell script to fix all decoder implementation files

$decoders = @(
    @{
        File = "src/decoder/mtype_decoder.cpp"
        HelperMethods = @("decodeLoad", "decodeStore")
        Class = "MTypeDecoder"
    },
    @{
        File = "src/decoder/btype_decoder.cpp"
        HelperMethods = @("decodeIPRelative", "decodeIndirect")
        Class = "BTypeDecoder"
    },
    @{
        File = "src/decoder/itype_decoder.cpp"
        HelperMethods = @("decodeMixedI", "decodeDepositExtract", "decodeShift")
        Class = "ITypeDecoder"
    }
)

foreach ($decoder in $decoders) {
    $filePath = "D:\devgitlab\guideXOS.Hypervisor\$($decoder.File)"
    Write-Host "Processing $($decoder.File)..."
    
    $content = Get-Content $filePath -Raw
    
    # Fix the backtick in include if present
    $content = $content -replace '#include "ia64_formats\.h"`n#include', '#include "ia64_formats.h"' + "`n" + '#include'
    
    # For each helper method, convert from class method to static function
    foreach ($method in $decoder.HelperMethods) {
        # Replace: bool ClassName::methodName( with: static bool methodName(
        $pattern = "bool $($decoder.Class)::$method\("
        $replacement = "static bool $method("
        $content = $content -replace $pattern, $replacement
    }
    
    # Add forward declarations after the comment block
    $forwardDecls = ""
    foreach ($method in $decoder.HelperMethods) {
        $forwardDecls += "static bool $method("
        # We need to get the signature, but for now just add a placeholder
    }
    
    Set-Content $filePath $content
    Write-Host "  Fixed $($decoder.File)"
}

Write-Host "`nAll decoders processed!"
