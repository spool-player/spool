param([Parameter(Mandatory)] [string] $ToolsRoot)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
$root = Get-RepositoryRoot
$logs = Join-Path $ToolsRoot 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$result = Join-Path $logs 'build-result.txt'
Remove-Item -LiteralPath $result -ErrorAction SilentlyContinue
Start-Transcript -Path (Join-Path $logs 'build.log') -Force
$exitCode = 1
try {
    $vc = @(Get-ChildItem "$ToolsRoot\msvc\VC\Tools\MSVC" -Directory)
    $sdkVersions = @(Get-ChildItem "$ToolsRoot\msvc\Windows Kits\10\Include" -Directory)
    $redist = @(Get-ChildItem "$ToolsRoot\msvc\VC\Redist\MSVC" -Directory | Where-Object Name -Match '^\d')
    if ($vc.Count -ne 1 -or $sdkVersions.Count -ne 1 -or $redist.Count -ne 1) {
        throw 'The pinned local MSVC installation must contain exactly one toolchain and SDK.'
    }
    $vc = $vc[0].FullName
    $sdk = "$ToolsRoot\msvc\Windows Kits\10"
    $sdkVersion = $sdkVersions[0].Name
    $env:VCToolsRedistDir = "$($redist[0].FullName)\"
    $env:MSYS2_LOCATION = "$ToolsRoot\msys-dated\msys64"
    $cmake = (Get-ChildItem "$ToolsRoot\cmake" -Directory | Select-Object -First 1).FullName
    $nasm = (Get-ChildItem "$ToolsRoot\nasm" -Directory | Select-Object -First 1).FullName
    $curl = (Get-ChildItem "$ToolsRoot\curl" -Directory | Select-Object -First 1).FullName
    $env:PATH = (@(
        "$vc\bin\Hostx64\x64", "$sdk\bin\$sdkVersion\x64",
        "$($env:VCToolsRedistDir)x64\Microsoft.VC143.CRT",
        "$cmake\bin", "$ToolsRoot\ninja", "$ToolsRoot\git\cmd",
        "$ToolsRoot\llvm\bin", $nasm, "$ToolsRoot\python",
        "$ToolsRoot\python\Scripts", "$curl\bin", "$ToolsRoot\imagemagick",
        $ToolsRoot, "$env:MSYS2_LOCATION\usr\bin", $env:PATH
    ) -join ';')
    $env:INCLUDE = "$vc\include;$sdk\Include\$sdkVersion\ucrt;$sdk\Include\$sdkVersion\shared;$sdk\Include\$sdkVersion\um;$sdk\Include\$sdkVersion\winrt;$sdk\Include\$sdkVersion\cppwinrt"
    $env:LIB = "$vc\lib\x64;$sdk\Lib\$sdkVersion\ucrt\x64;$sdk\Lib\$sdkVersion\um\x64"
    Set-Location $root
    $mesonWheel = Get-ChildItem "$ToolsRoot\wheels\meson-*.whl" | Select-Object -First 1
    if (-not (Test-Path "$ToolsRoot\python\Scripts\meson.exe")) {
        & python -m pip install --no-index $mesonWheel.FullName
        if ($LASTEXITCODE -ne 0) { throw 'Installing pinned Meson failed.' }
    }
    # CMake's default /Zi probe needs mspdbsrv RPC, unavailable under Wine.
    # These change debug storage only; build.ps1 still verifies /GL and /LTCG.
    $cmakeArguments = @('-DCMAKE_POLICY_DEFAULT_CMP0141=NEW', '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded')
    & (Join-Path $PSScriptRoot 'install-qt.ps1')
    & (Join-Path $PSScriptRoot 'install-qcoro.ps1') -CMakeArguments $cmakeArguments
    & (Join-Path $PSScriptRoot 'build.ps1') -CMakeArguments $cmakeArguments *> (Join-Path $logs 'application-build.log')
    & (Join-Path $PSScriptRoot 'stage.ps1')
    $exitCode = 0
} catch {
    Write-Host ($_ | Out-String)
} finally {
    [IO.File]::WriteAllText($result, "$exitCode`n")
    Stop-Transcript
}
exit $exitCode
