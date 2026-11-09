# One-off: compiles src\skeuomorph\SpikeCurl.hlsl into byte arrays (SpikeCurlShaders.h) with the Windows SDK's fxc.
$fxc = (Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe" | Sort-Object FullName | Select-Object -Last 1).FullName
$src = "$PSScriptRoot\..\..\src\skeuomorph"
& $fxc /nologo /T vs_4_0 /E VS /O3 /Vn gSpikeVS /Fh "$src\vs.tmp" "$src\SpikeCurl.hlsl"
& $fxc /nologo /T ps_4_0 /E PS /O3 /Vn gSpikePS /Fh "$src\ps.tmp" "$src\SpikeCurl.hlsl"
Get-Content "$src\vs.tmp", "$src\ps.tmp" | Set-Content -Encoding ascii "$src\SpikeCurlShaders.h"
Remove-Item "$src\vs.tmp", "$src\ps.tmp"
