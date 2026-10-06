# Builds capi_test.c against the release fak_capi.lib with MSVC and runs it on (fak, wav) pairs.
# usage: pwsh capi/tests/run.ps1 <file.wav> [more.wav ...]
# Each WAV is encoded with the fak.exe that build.ps1 built first.
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Wavs)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Resolve-Path "$here\..\.."
$repo = "$root\build\fak-cli"
$out = "$root\build\capi_test"
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath  # -products *: Build Tools too
$libs = "psapi.lib shell32.lib user32.lib advapi32.lib bcrypt.lib kernel32.lib ntdll.lib userenv.lib ws2_32.lib dbghelp.lib"
$cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /O2 /MD /W4 /I`"$root\capi\include`" `"$here\capi_test.c`" /Fe`"$out\capi_test.exe`" /Fo`"$out\\`" `"$root\target\release\fak_capi.lib`" $libs"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

$fail = 0
foreach ($w in $Wavs) {
    $f = "$out\t.fak"
    & "$repo\bin\fak.exe" encode $w $f | Out-Null
    Write-Host "== $w"
    & "$out\capi_test.exe" $f $w
    if ($LASTEXITCODE -ne 0) { $fail++ }
}
if ($fail) { throw "$fail test run(s) failed" }
Write-Host "all C API tests passed"
