# Bygger BerntTracker (ft2-clone) i Release x64 med MSBuild fra Visual Studio,
# og starter programmet etterpaa hvis -Start er med.
param([switch]$Start)

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'release\win64\ft2-clone-win64.exe'

# Lukk programmet hvis det kjorer, ellers kan ikke den nye versjonen lagres
Get-Process -Name 'ft2-clone-win64' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Host 'FEIL: Fant ikke Visual Studio. Er Visual Studio 2026 installert?'
    exit 1
}

$msbuild = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    Write-Host 'FEIL: Fant ikke MSBuild. Installer "Desktop development with C++" i Visual Studio Installer.'
    exit 1
}

Write-Host 'Bygger... (forste gang kan det ta et minutt)'
& $msbuild (Join-Path $root 'vs2026_project\ft2-clone\ft2-clone.vcxproj') /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) {
    Write-Host ''
    Write-Host 'BYGGING FEILET. Kopier feilmeldingene over og send dem til Claude.'
    exit $LASTEXITCODE
}

Write-Host ''
Write-Host 'Ferdig bygget.'

if ($Start) {
    Write-Host 'Starter programmet...'
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe)
}
exit 0
