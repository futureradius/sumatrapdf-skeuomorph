# Runs one automatic test: .\run-one.ps1 <label> <pdf: scan|text> <options>
#   e.g. .\run-one.ps1 T1 text "mode=tap,rough=1,size=1920x1200"
# The window stays on top for up to a minute or two, then SumatraPDF closes itself and the result is printed.
#   add "rel" as a 4th argument to run the release build (out\rel64) instead of the debug build
param([string]$label, [string]$pdf, [string]$opts, [string]$cfg = "dbg")
$root = Resolve-Path "$PSScriptRoot\..\.."
$res = "$PSScriptRoot\results"
New-Item -ItemType Directory -Force $res | Out-Null
if (-not (Test-Path "$PSScriptRoot\$pdf.pdf")) { & "$PSScriptRoot\make-test-pdfs.ps1" }
$env:SPIKE_ROUGH = "$opts,out=$res,label=$label"
$p = Start-Process "$root\out\${cfg}64\SumatraPDF.exe" -ArgumentList "-for-testing", "$PSScriptRoot\$pdf.pdf" -WorkingDirectory $root -PassThru
if (-not $p.WaitForExit(240000)) { $p.Kill(); "timed out" }
$env:SPIKE_ROUGH = $null
Start-Sleep -Milliseconds 300
Get-Content "$res\$label.txt"
