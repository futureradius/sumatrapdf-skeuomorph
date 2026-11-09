# Runs every measurement and writes results\*.txt and screenshots. Takes about 2 minutes.
# Don't touch the mouse or keyboard while it runs.
$here = $PSScriptRoot
$exe = Join-Path $here "curl-layer-spike.exe"
$out = Join-Path $here "results"
New-Item -ItemType Directory -Force $out | Out-Null

& $exe --out $out --label hw-maximized --seconds 5
& $exe --out $out --label hw-1920x1200 --size 1920x1200 --seconds 5
& $exe --out $out --label warp-maximized --warp --seconds 5
& $exe --out $out --label warp-1920x1200 --warp --size 1920x1200 --seconds 5
& $exe --out $out --label warp-1280x800 --warp --size 1280x800 --seconds 5
& $exe --out $out --label mx550-maximized --adapter 1 --seconds 5
