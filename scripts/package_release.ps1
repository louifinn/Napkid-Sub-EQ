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

# The bundle's moduleinfo must agree with the .jucer version; a stale bundle
# would otherwise ship a version the host reports differently from the zip.
$moduleInfoPath = Join-Path $vst3 "Contents/Resources/moduleinfo.json"
if (Test-Path $moduleInfoPath)
{
    # JUCE writes moduleinfo.json with a trailing comma (not strict JSON), so
    # ConvertFrom-Json rejects it; extract the version field directly.
    $moduleInfoText = Get-Content -Raw $moduleInfoPath
    if ($moduleInfoText -notmatch '"Version"\s*:\s*"([^"]+)"')
    {
        throw "Could not read Version from $moduleInfoPath"
    }
    $moduleVersion = $Matches[1]
    if ($moduleVersion -ne $version)
    {
        throw "VST3 moduleinfo version $moduleVersion does not match .jucer version $version"
    }
}
else
{
    throw "VST3 moduleinfo.json not found: $moduleInfoPath"
}

# Signing is an external release step (requires a code-signing certificate).
# Report the current state loudly so an unsigned build is never mistaken for
# a signed release.
$vst3Dll = Join-Path $vst3 "Contents/x86_64-win/Napkid Sub EQ.vst3"
foreach ($binary in @($standalone, $vst3Dll))
{
    if (Test-Path $binary)
    {
        $sig = Get-AuthenticodeSignature $binary
        if ($sig.Status -ne "Valid")
        {
            Write-Warning "Unsigned/invalid signature: $binary ($($sig.Status))"
        }
    }
}

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

$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
$shaFile = "$zip.sha256"
# LF + ASCII (no BOM/CRLF) so `sha256sum -c` works unchanged.
[System.IO.File]::WriteAllText(
    $shaFile,
    "$hash  $(Split-Path -Leaf $zip)`n",
    (New-Object System.Text.ASCIIEncoding))
Write-Host "Packaged: $zip"
Write-Host "SHA256:   $hash"
Write-Host "Checksum: $shaFile"
