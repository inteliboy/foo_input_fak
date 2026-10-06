# Builds foo_input_fak.dll (x64) and packages foo_input_fak.fb2k-component.
#   1. the Rust C-ABI static library (capi/ -> target/release/fak_capi.lib)
#   2. the fak.exe CLI built from the pinned FAK-Codec revision (for the converter's custom-encoder preset)
#   3. the C++ component against the foobar2000 SDK (MSBuild, VS 2022 toolset v143)
# Usage: pwsh build.ps1 [-Configuration Release] [-Fb2kSdk <path to the unpacked foobar2000 SDK; default .\sdk>]
param(
    [string]$Configuration = "Release",
    [string]$Fb2kSdk = ""
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$fakRev = "3aa1f1f97339bd25032fcd1b8234d6111457fd25"

Push-Location "$here\capi"
try {
    $env:CARGO_TARGET_DIR = "$here\target"
    cargo build --release
    if ($LASTEXITCODE -ne 0) { throw "cargo build (fak_capi) failed" }
} finally { Pop-Location; Remove-Item Env:\CARGO_TARGET_DIR -ErrorAction SilentlyContinue }

cargo install --git https://github.com/inteliboy/FAK-Codec --rev $fakRev --locked --force --root "$here\build\fak-cli" fak
if ($LASTEXITCODE -ne 0) { throw "cargo install (fak CLI) failed" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found (install Visual Studio 2022 with the C++ workload)" }
$props = @("/p:Configuration=$Configuration", "/p:Platform=x64", "/m", "/nologo", "/v:minimal")
if ($Fb2kSdk) { $props += "/p:Fb2kSdk=$((Resolve-Path $Fb2kSdk).Path)\" }
& $msbuild "$here\src\foo_input_fak.vcxproj" @props
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed" }

# A .fb2k-component is a zip; x64 binaries go in an x64\ subfolder.
$out = "$here\build\$Configuration"
$stage = "$out\stage"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\x64" | Out-Null
Copy-Item "$here\build\x64\$Configuration\foo_input_fak.dll" "$stage\x64\"
Copy-Item "$here\build\fak-cli\bin\fak.exe" "$stage\x64\"
$pkg = "$out\foo_input_fak.fb2k-component"
Remove-Item $pkg -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath "$out\foo_input_fak.zip" -Force
Move-Item "$out\foo_input_fak.zip" $pkg -Force
Write-Host "Built $pkg"
