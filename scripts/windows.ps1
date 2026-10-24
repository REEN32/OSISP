param([string]$Output = "data/windows")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)
function Check { if ($LASTEXITCODE -ne 0) { throw "Command failed: $LASTEXITCODE" } }
cmake -S . -B build-windows -A x64
Check
cmake --build build-windows --config Release
Check
ctest --test-dir build-windows -C Release --output-on-failure
Check
python -m pip install -r scripts/requirements.txt
Check
python scripts/experiments.py --exe build-windows/Release/philosophers.exe --out $Output
Check
python scripts/analyze.py --data $Output
Check
python scripts/build_report.py --data $Output
Check
Write-Host "Ready: report/report.md, report/report.docx; source measurements: $Output"
