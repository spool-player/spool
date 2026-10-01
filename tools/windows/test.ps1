. (Join-Path $PSScriptRoot 'common.ps1')
Initialize-WindowsBuildEnvironment
$root = Get-RepositoryRoot
$testPath = "$(Join-Path $env:SPOOL_MPV_ROOT 'bin');$(Join-Path $env:SPOOL_QT_ROOT 'bin');$env:PATH"

cmake --build (Join-Path $root 'build\windows-release\app')
if ($LASTEXITCODE -ne 0) { throw 'Building Windows tests failed.' }
cmake -E env "PATH=$testPath" ctest --test-dir (Join-Path $root 'build\windows-release\app') `
    --parallel ([Environment]::ProcessorCount) --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Windows tests failed.' }
