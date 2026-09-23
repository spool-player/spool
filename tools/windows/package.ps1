param(
    [string] $StageDirectory,
    [string] $OutputDirectory,
    [string] $MakeNsis
)

. (Join-Path $PSScriptRoot 'common.ps1')
$root = Get-RepositoryRoot
$stage = if ($StageDirectory) { [IO.Path]::GetFullPath($StageDirectory) } else {
    Join-Path $root 'build\windows-release\stage'
}
$output = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $root 'dist' }

foreach ($relative in @('spool.exe', 'mpv-2.dll', 'imageformats\qwebp.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $relative))) {
        throw "The staged Windows payload is incomplete: $relative"
    }
}

$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION is not a three-part semantic version: $version"
}

$fingerprintLines = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stage.TrimEnd('\').Length + 1).Replace('\', '/')
        "$relative`:$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
    }
$fingerprintBytes = [Text.Encoding]::UTF8.GetBytes(($fingerprintLines -join "`n"))
$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $payloadId = ([BitConverter]::ToString($sha256.ComputeHash($fingerprintBytes))).Replace('-', '').Substring(0, 16).ToLowerInvariant()
} finally {
    $sha256.Dispose()
}

if (-not $MakeNsis) {
    $command = Get-Command makensis.exe -ErrorAction SilentlyContinue
    $candidates = @()
    if ($command) { $candidates += $command.Source }
    if ($env:NSIS_ROOT) { $candidates += Join-Path $env:NSIS_ROOT 'makensis.exe' }
    $candidates += Join-Path $root 'build\windows-tools\nsis\makensis.exe'
    $candidates += Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'
    $MakeNsis = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $MakeNsis -or -not (Test-Path -LiteralPath $MakeNsis)) {
    throw 'makensis.exe was not found. Install NSIS 3 or set NSIS_ROOT.'
}

New-Item -ItemType Directory -Force $output | Out-Null
$portable = Join-Path $output "Spool-$version-Windows-x64-Portable.exe"
$installer = Join-Path $output "Spool-$version-Windows-x64-Setup.exe"
Remove-Item -LiteralPath $portable, $installer -Force -ErrorAction SilentlyContinue

function Invoke-NsisPackage {
    param([string] $Script, [string] $Artifact)

    $arguments = @(
        '/V2',
        '/NOCD',
        "/DVERSION=$version",
        "/DPAYLOAD_ID=$payloadId",
        "/DSTAGE_DIR=$stage",
        "/DSOURCE_ROOT=$root",
        "/DOUTPUT_FILE=$Artifact",
        (Join-Path $PSScriptRoot $Script)
    )
    & $MakeNsis @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "NSIS failed to build $Script"
    }
    if (-not (Test-Path -LiteralPath $Artifact)) {
        throw "NSIS did not create the expected artifact: $Artifact"
    }
}

Invoke-NsisPackage -Script 'portable.nsi' -Artifact $portable
Invoke-NsisPackage -Script 'installer.nsi' -Artifact $installer

$sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
$stageFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    [PSCustomObject]@{
        Path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/')
        Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        Size = $_.Length
    }
})
$stageLogicalBytes = ($stageFiles | Measure-Object Size -Sum).Sum
foreach ($artifact in @($portable, $installer)) {
    $extractRoot = Join-Path $env:TEMP "spool-package-audit-$PID-$([IO.Path]::GetFileNameWithoutExtension($artifact))"
    Remove-Item -LiteralPath $extractRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $extractRoot | Out-Null
    try {
        & $sevenZip x -y "-o$extractRoot" $artifact | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip could not extract $artifact" }
        $extractedFiles = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File | ForEach-Object {
            [PSCustomObject]@{
                File = $_
                Path = [IO.Path]::GetRelativePath($extractRoot, $_.FullName).Replace('\', '/')
                Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        })
        $matched = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($expected in $stageFiles) {
            $suffix = "/$($expected.Path)"
            $candidates = @($extractedFiles | Where-Object {
                $_.Path -eq $expected.Path -or $_.Path.EndsWith($suffix, [StringComparison]::OrdinalIgnoreCase)
            })
            if (-not ($candidates | Where-Object Hash -EQ $expected.Hash)) {
                throw "$artifact does not contain the staged path/hash $($expected.Path)"
            }
            foreach ($candidate in $candidates | Where-Object Hash -EQ $expected.Hash) {
                [void] $matched.Add($candidate.File.FullName)
            }
        }
        $wrapperBytes = ($extractedFiles | Where-Object { -not $matched.Contains($_.File.FullName) } |
            ForEach-Object { $_.File.Length } | Measure-Object -Sum).Sum
        Write-Host "$([IO.Path]::GetFileName($artifact)): stage logical bytes=$stageLogicalBytes; extracted wrapper bytes=$wrapperBytes; compressed bytes=$((Get-Item -LiteralPath $artifact).Length)"
    } finally {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Get-Item -LiteralPath $portable, $installer |
    Select-Object Name, Length, @{ Name = 'MiB'; Expression = { [math]::Round($_.Length / 1MB, 2) } }
