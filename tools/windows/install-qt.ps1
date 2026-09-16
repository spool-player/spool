param(
    [string] $OutputDirectory = 'C:\Qt',
    [string] $DownloadDirectory,
    [switch] $Force
)

. (Join-Path $PSScriptRoot 'common.ps1')
$root = Get-RepositoryRoot
$manifestPath = Join-Path $root 'tools\manifests\qt-windows-6.11.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$qtRoot = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) "$($manifest.version)\$($manifest.architecture)"
$downloads = if ($DownloadDirectory) { [IO.Path]::GetFullPath($DownloadDirectory) } elseif ($env:RUNNER_TEMP) {
    Join-Path $env:RUNNER_TEMP 'jellyfin-qt-downloads'
} else {
    Join-Path $root 'build\windows-deps\qt-downloads'
}

function Test-QtInstallation {
    return (Test-Path -LiteralPath (Join-Path $qtRoot 'bin\qmake.exe')) -and
        (Test-Path -LiteralPath (Join-Path $qtRoot 'lib\cmake\Qt6\Qt6Config.cmake')) -and
        (Test-Path -LiteralPath (Join-Path $qtRoot 'lib\cmake\Qt6WebSockets\Qt6WebSocketsConfig.cmake')) -and
        (Test-Path -LiteralPath (Join-Path $qtRoot 'lib\cmake\Qt6Svg\Qt6SvgConfig.cmake')) -and
        (Test-Path -LiteralPath (Join-Path $qtRoot 'lib\cmake\Qt6ShaderTools\Qt6ShaderToolsConfig.cmake')) -and
        (Test-Path -LiteralPath (Join-Path $qtRoot 'plugins\imageformats\qwebp.dll'))
}

if ((Test-QtInstallation) -and -not $Force) {
    Write-Host "Qt $($manifest.version) is already installed at $qtRoot"
    return
}

if ($Force -and (Test-Path -LiteralPath $qtRoot)) {
    $outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
    $resolvedQtRoot = [IO.Path]::GetFullPath($qtRoot)
    if (-not $resolvedQtRoot.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a Qt path outside the requested output directory: $qtRoot"
    }
    Remove-Item -LiteralPath $qtRoot -Recurse -Force
}

New-Item -ItemType Directory -Force $qtRoot, $downloads | Out-Null
$sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source

foreach ($archive in $manifest.archives) {
    $archiveName = "$($manifest.packageVersion)$($archive.file)"
    $archivePath = Join-Path $downloads $archiveName
    $expectedHash = $archive.sha1.ToLowerInvariant()
    $validDownload = (Test-Path -LiteralPath $archivePath) -and
        ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA1).Hash.ToLowerInvariant() -eq $expectedHash)

    if (-not $validDownload) {
        $partialPath = "$archivePath.part"
        Remove-Item -LiteralPath $partialPath -Force -ErrorAction SilentlyContinue
        $url = "$($manifest.repository)/$($archive.package)/$archiveName"
        Write-Host "Downloading $($archive.file)"
        & curl.exe -L --fail --retry 4 --retry-all-errors --connect-timeout 15 --max-time 900 `
            --output $partialPath $url
        if ($LASTEXITCODE -ne 0) { throw "Failed to download $url" }
        $actualHash = (Get-FileHash -LiteralPath $partialPath -Algorithm SHA1).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Checksum mismatch for $($archive.file): expected $expectedHash, got $actualHash"
        }
        Move-Item -LiteralPath $partialPath -Destination $archivePath -Force
    }

    Write-Host "Extracting $($archive.file)"
    & $sevenZip x -y "-o$qtRoot" $archivePath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract $archivePath" }
}

$qtConf = "[Paths]`r`nPrefix=..`r`n"
[IO.File]::WriteAllText((Join-Path $qtRoot 'bin\qt.conf'), $qtConf, [Text.UTF8Encoding]::new($false))

if (-not (Test-QtInstallation)) {
    throw "The extracted Qt installation is incomplete: $qtRoot"
}
Write-Host "Installed Qt $($manifest.version) at $qtRoot"
