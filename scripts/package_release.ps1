# Builds a distributable zip for Napkid Sub EQ.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/package_release.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$jucerPath = Join-Path $root "Napkid Sub EQ.jucer"
[xml]$jucer = Get-Content -Raw $jucerPath
$version = $jucer.JUCERPROJECT.version
$release = Join-Path $root "Builds/VisualStudio2022/x64/Release"
$vst3 = Join-Path $release "VST3/Napkid Sub EQ.vst3"
$standalone = Join-Path $release "Standalone Plugin/Napkid Sub EQ.exe"
$dist = Join-Path $root "dist/NapkidSubEQ-$version"
$zip = "$dist.zip"

if (-not (Test-Path $vst3)) { throw "VST3 bundle not found: $vst3" }
if (-not (Test-Path $standalone)) { throw "Standalone exe not found: $standalone" }

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Path $dist | Out-Null
New-Item -ItemType Directory -Path (Join-Path $dist "VST3") | Out-Null
Copy-Item -Recurse -Force $vst3 (Join-Path $dist "VST3")
Copy-Item -Force $standalone $dist
Copy-Item -Force (Join-Path $root "README.md") $dist
Copy-Item -Force (Join-Path $root "README.en.md") $dist
Copy-Item -Force (Join-Path $root "LICENSE") $dist
Copy-Item -Force (Join-Path $root "THIRD_PARTY_NOTICES.md") $dist
Copy-Item -Force (Join-Path $root "CHANGELOG.md") $dist
Copy-Item -Force (Join-Path $root "Source/Fonts/OFL.txt") $dist

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip
Write-Host "Packaged: $zip"
