# Fix the malformed vcxproj file

$content = Get-Content "guideXOS Hypervisor Core.vcxproj"
$newContent = @()
$skipLines = $false

foreach ($line in $content) {
    # When we find mtype_decoder, add it and the new entries
    if ($line -match 'src\\decoder\\mtype_decoder\.cpp') {
        $newContent += $line
        $newContent += '    </ClCompile>'
        $newContent += '    <ClCompile Include="src\decoder\ftype_decoder.cpp">'
        $newContent += '    </ClCompile>'
        $newContent += '    <ClCompile Include="src\decoder\xtype_decoder.cpp">'
        $skipLines = $true
        continue
    }
    
    # Skip malformed closing tags until we find Fuzzer
    if ($skipLines) {
        if ($line -match 'src\\fuzzer\\Fuzzer\.cpp') {
            $skipLines = $false
            $newContent += $line
        }
        continue
    }
    
    $newContent += $line
}

$newContent | Set-Content "guideXOS Hypervisor Core.vcxproj" -Encoding UTF8
Write-Host "Fixed guideXOS Hypervisor Core.vcxproj"
