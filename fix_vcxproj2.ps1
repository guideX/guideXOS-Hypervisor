# Fix vcxproj - second pass

$content = Get-Content "guideXOS Hypervisor Core.vcxproj" -Raw

# Fix the xtype_decoder closing tag
$content = $content -replace '(<ClCompile Include="src\\decoder\\xtype_decoder\.cpp">)\s*(<ClCompile Include="src\\fuzzer\\Fuzzer\.cpp">)', '$1' + "`r`n    </ClCompile>`r`n    " + '$2'

Set-Content "guideXOS Hypervisor Core.vcxproj" -Value $content -NoNewline
Write-Host "Fixed xtype_decoder closing tag"
