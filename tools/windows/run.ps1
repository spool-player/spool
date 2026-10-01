. (Join-Path $PSScriptRoot 'common.ps1')
$root = Get-RepositoryRoot
$exe = Join-Path $root 'build\windows-release\stage\spool.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Staged executable was not found: $exe"
}
Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -Wait
