# Runs one automated test: .\run-one.ps1 <label> <options>   e.g. .\run-one.ps1 A "layer=ondemand,wait=0,flush=1,copy=ui"
param([string]$label, [string]$opts, [string]$size = "1600x1000")
$root = Resolve-Path "$PSScriptRoot\..\.."
$res = "$PSScriptRoot\results"
New-Item -ItemType Directory -Force $res | Out-Null
if (-not (Test-Path "$PSScriptRoot\test.pdf")) { python "$PSScriptRoot\make-test-pdf.py" "$PSScriptRoot\test.pdf" }
$env:SPIKE_NOFLASH = "$opts,size=$size,out=$res,label=$label"
$p = Start-Process "$root\out\dbg64\SumatraPDF.exe" -ArgumentList "-for-testing", "$PSScriptRoot\test.pdf" -WorkingDirectory $root -PassThru
if (-not $p.WaitForExit(180000)) { $p.Kill(); "timed out" }
Start-Sleep -Milliseconds 300
Get-Content "$res\$label.txt"
