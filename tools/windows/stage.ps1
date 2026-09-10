. (Join-Path $PSScriptRoot 'common.ps1')
Initialize-WindowsBuildEnvironment
$root = Get-RepositoryRoot
$buildDir = Join-Path $root 'build\windows-release\app'
$stageDir = Join-Path $root 'build\windows-release\stage'
$exe = Join-Path $buildDir 'jellyfin-native.exe'

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Release executable was not found: $exe"
}

if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stageDir | Out-Null
Copy-Item -LiteralPath $exe -Destination $stageDir
$fontDir = Join-Path $stageDir 'fonts'
New-Item -ItemType Directory -Path $fontDir | Out-Null
foreach ($fontName in @(
    'AtkinsonHyperlegible-Bold.otf',
    'AtkinsonHyperlegible-Regular.otf',
    'IBMPlexSans-Variable.ttf',
    'PTRootUI-Variable.ttf',
    'MaterialIcons-Regular.ttf'
)) {
    Copy-Item -LiteralPath (Join-Path $root "qml\fonts\$fontName") -Destination $fontDir
}

& (Join-Path $env:JELLYFIN_QT_ROOT 'bin\windeployqt.exe') `
    --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler `
    --no-compiler-runtime --no-opengl-sw `
    --skip-plugin-types qmltooling,generic `
    --include-plugins qwebp `
    --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql `
    --qmldir (Join-Path $root 'qml') (Join-Path $stageDir 'jellyfin-native.exe')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }

$foreignStylePaths = @(
    'qml\QtQuick\Controls\FluentWinUI3',
    'qml\QtQuick\Controls\Fusion',
    'qml\QtQuick\Controls\Imagine',
    'qml\QtQuick\Controls\Material',
    'qml\QtQuick\Controls\Universal',
    'qml\QtQuick\Dialogs\quickimpl\qml\+FluentWinUI3',
    'qml\QtQuick\Dialogs\quickimpl\qml\+Fusion',
    'qml\QtQuick\Dialogs\quickimpl\qml\+Imagine',
    'qml\QtQuick\Dialogs\quickimpl\qml\+Material',
    'qml\QtQuick\Dialogs\quickimpl\qml\+Universal'
)
foreach ($relativePath in $foreignStylePaths) {
    $path = Join-Path $stageDir $relativePath
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
$dialogsQmldir = Join-Path $stageDir 'qml\QtQuick\Dialogs\quickimpl\qmldir'
if (Test-Path -LiteralPath $dialogsQmldir) {
    $filteredQmldir = Get-Content -LiteralPath $dialogsQmldir |
        Where-Object { $_ -notmatch 'qml/\+(FluentWinUI3|Fusion|Imagine|Material|Universal)/' }
    Set-Content -LiteralPath $dialogsQmldir -Value $filteredQmldir -Encoding utf8NoBOM
}
foreach ($pattern in @(
    'Qt6QuickControls2FluentWinUI3*.dll',
    'Qt6QuickControls2Fusion*.dll',
    'Qt6QuickControls2Imagine*.dll',
    'Qt6QuickControls2Material*.dll',
    'Qt6QuickControls2Universal*.dll'
)) {
    Get-ChildItem -LiteralPath $stageDir -Filter $pattern -File |
        Remove-Item -Force
}
Get-ChildItem -LiteralPath $stageDir -Filter '*.qmltypes' -File -Recurse |
    Remove-Item -Force

$webpPlugin = Join-Path $stageDir 'imageformats\qwebp.dll'
if (-not (Test-Path -LiteralPath $webpPlugin)) {
    throw 'Qt WebP support was not deployed. Reinstall the pinned Qt imageformats archives with tools\windows\install-qt.ps1 -Force.'
}

$mpvBin = Join-Path $env:JELLYFIN_MPV_ROOT 'bin'
$mpvCandidates = @(Get-ChildItem -LiteralPath $mpvBin -Filter '*mpv*.dll' -File)
$mpvDll = $mpvCandidates | Where-Object Name -EQ 'mpv-2.dll' | Select-Object -First 1
if (-not $mpvDll) { $mpvDll = $mpvCandidates | Select-Object -First 1 }
if (-not $mpvDll) { throw "libmpv DLL was not found below $mpvBin" }
Copy-Item -LiteralPath $mpvDll.FullName -Destination (Join-Path $stageDir 'mpv-2.dll')

$crtRoot = Join-Path $env:VCToolsRedistDir 'x64'
$crtDirectory = Get-ChildItem -LiteralPath $crtRoot -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $crtDirectory) {
    throw "The app-local MSVC runtime was not found below $crtRoot"
}

$externalProviders = @{}
foreach ($directory in @($mpvBin, $crtDirectory.FullName)) {
    foreach ($candidate in Get-ChildItem -LiteralPath $directory -Filter '*.dll' -File) {
        $key = $candidate.Name.ToLowerInvariant()
        if (-not $externalProviders.ContainsKey($key)) {
            $externalProviders[$key] = [Collections.Generic.List[IO.FileInfo]]::new()
        }
        $externalProviders[$key].Add($candidate)
    }
}
$externalDuplicates = @($externalProviders.GetEnumerator() | Where-Object { $_.Value.Count -gt 1 })
if ($externalDuplicates.Count -gt 0) {
    $details = $externalDuplicates | Sort-Object Key | ForEach-Object {
        "$($_.Key): $(($_.Value.FullName) -join ', ')"
    }
    throw "Runtime search roots have duplicate same-name DLL providers:`n$($details -join "`n")"
}

$dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
$stagedProviders = @{}
$pending = [Collections.Generic.Queue[string]]::new()
$runtimeRoots = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($binary in Get-ChildItem -LiteralPath $stageDir -Recurse -File |
    Where-Object Extension -In @('.dll', '.exe')) {
    $key = $binary.Name.ToLowerInvariant()
    if ($stagedProviders.ContainsKey($key)) {
        throw "The staged Windows payload has duplicate same-name providers: $($binary.Name)"
    }
    $stagedProviders[$key] = $binary.FullName
    if ($binary.Name -in @('jellyfin-native.exe', 'mpv-2.dll') -or
        $binary.DirectoryName -ne $stageDir) {
        [void] $runtimeRoots.Add($binary.FullName)
    }
}
foreach ($runtimeRoot in $runtimeRoots) { $pending.Enqueue($runtimeRoot) }
$inspected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$missing = [Collections.Generic.List[string]]::new()
while ($pending.Count -gt 0) {
    $binary = $pending.Dequeue()
    if (-not $inspected.Add($binary)) { continue }
    $relative = [IO.Path]::GetRelativePath($stageDir, $binary)
    $imports = @(& $dumpbin /nologo /dependents $binary | ForEach-Object {
        if ($_ -match '^\s+([^\s]+\.dll)\s*$') { $Matches[1] }
    } | Sort-Object -Unique)
    if ($LASTEXITCODE -ne 0) { throw "dumpbin could not inspect $binary" }
    foreach ($import in $imports) {
        $key = $import.ToLowerInvariant()
        if ($stagedProviders.ContainsKey($key)) {
            $pending.Enqueue($stagedProviders[$key])
            continue
        }
        if ($import -match '^(api|ext)-ms-win-') { continue }
        $isCompilerRuntime = $import -match '^(concrt|msvcp|vcruntime)\d*(_\d+)?\.dll$'
        if (-not $isCompilerRuntime -and
            (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$import"))) {
            continue
        }
        $provider = $externalProviders[$key]
        if ($null -eq $provider) {
            $missing.Add("$relative -> $import")
            continue
        }
        $destination = Join-Path $stageDir $import
        Copy-Item -LiteralPath $provider[0].FullName -Destination $destination
        $stagedProviders[$key] = $destination
        $pending.Enqueue($destination)
    }
}
if ($missing.Count -gt 0) {
    throw "The Windows payload has missing runtime dependencies:`n$($missing -join "`n")"
}

$licenseDir = Join-Path $stageDir 'licenses'
New-Item -ItemType Directory -Path $licenseDir | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'app\notices\OPEN_SOURCE_NOTICES.txt') `
    -Destination (Join-Path $licenseDir 'OPEN_SOURCE_NOTICES.txt')
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') `
    -Destination (Join-Path $licenseDir 'MPL-2.0.txt')
Copy-Item -LiteralPath (Join-Path $root 'qml\fonts\AtkinsonHyperlegible-LICENSE.txt') `
    -Destination (Join-Path $licenseDir 'AtkinsonHyperlegible-OFL.txt')
Copy-Item -LiteralPath (Join-Path $root 'qml\fonts\IBMPlexSans-LICENSE.txt') `
    -Destination (Join-Path $licenseDir 'IBMPlexSans-OFL.txt')
Copy-Item -LiteralPath (Join-Path $root 'qml\fonts\PTRootUI-LICENSE.txt') `
    -Destination (Join-Path $licenseDir 'PTRootUI-OFL.txt')
Copy-Item -LiteralPath (Join-Path $root 'qml\fonts\MaterialIcons-LICENSE.txt') `
    -Destination (Join-Path $licenseDir 'MaterialIcons-Apache-2.0.txt')

$closureOutput = @(& (Join-Path $PSScriptRoot 'test-runtime-closure.ps1') `
    -StageDirectory $stageDir -AllowOrphans)
foreach ($line in $closureOutput) {
    if ($line -match "^ORPHAN`t(.+)$") {
        Remove-Item -LiteralPath (Join-Path $stageDir $Matches[1]) -Force
    }
}
& (Join-Path $PSScriptRoot 'test-runtime-closure.ps1') -StageDirectory $stageDir

$ffmpegAuditArguments = @(
    (Join-Path $root 'tools\ffmpeg-capabilities.py'),
    '--manifest',
    (Join-Path $root 'tools\manifests\ffmpeg-capabilities.json'),
    'audit-closure',
    $stageDir
)
if (Get-Command py.exe -ErrorAction SilentlyContinue) {
    & py.exe -3 @ffmpegAuditArguments
} elseif (Get-Command python.exe -ErrorAction SilentlyContinue) {
    & python.exe @ffmpegAuditArguments
} else {
    throw 'A system Python interpreter is required for the FFmpeg dependency closure audit.'
}
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg dependency closure audit failed.' }


$launchRoot = Join-Path $env:TEMP "spool-stage-launch-$PID"
$env:LOCALAPPDATA = Join-Path $launchRoot 'data'
$env:JELLYFIN_NATIVE_CACHE_HOME = Join-Path $launchRoot 'cache'
$env:JELLYFIN_DIAGNOSTICS_DIR = Join-Path $launchRoot 'diagnostics'
$launchProcess = Start-Process -FilePath (Join-Path $stageDir 'jellyfin-native.exe') `
    -ArgumentList '--launch-test' -WorkingDirectory $stageDir -PassThru
if (-not $launchProcess.WaitForExit(45000)) {
    Stop-Process -Id $launchProcess.Id -Force
    throw 'The staged Windows executable did not exit its UI launch test within 45 seconds.'
}
$launchProcess.Refresh()
if ($launchProcess.ExitCode -ne 0) {
    $launchLog = Join-Path $env:LOCALAPPDATA 'spool-jellyfin\logs\jellyfin-native.log'
    if (Test-Path -LiteralPath $launchLog) {
        Write-Host '--- staged executable launch log ---'
        Get-Content -LiteralPath $launchLog
        Write-Host '--- end staged executable launch log ---'
    }
    throw "The staged Windows executable failed its UI launch test with exit code $($launchProcess.ExitCode)."
}
Write-Host 'Staged executable UI launch test passed.'

Write-Host "Staged Windows release: $stageDir"
