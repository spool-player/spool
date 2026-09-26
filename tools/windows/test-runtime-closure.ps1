param(
    [Parameter(Mandatory)] [string] $StageDirectory,
    [string[]] $RuntimeLoadedRoot = @(),
    [switch] $AllowOrphans
)

. (Join-Path $PSScriptRoot 'common.ps1')
Import-MsvcEnvironment

$stage = [IO.Path]::GetFullPath($StageDirectory)
if (-not (Test-Path -LiteralPath $stage -PathType Container)) {
    throw "The staged Windows payload was not found: $stage"
}

$dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
$binaries = @(Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object Extension -In @('.dll', '.exe') |
    Sort-Object FullName)
$applicationExecutable = Join-Path $stage 'spool.exe'
$applicationHeaders = @(& $dumpbin /nologo /headers $applicationExecutable)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin could not inspect $applicationExecutable"
}
if (-not ($applicationHeaders -match '^\s*2\s+subsystem \(Windows GUI\)\s*$')) {
    throw 'spool.exe must use the Windows GUI subsystem so packaged launches do not open a terminal.'
}

$providers = @{}
foreach ($binary in $binaries) {
    $name = $binary.Name.ToLowerInvariant()
    if (-not $providers.ContainsKey($name)) {
        $providers[$name] = [Collections.Generic.List[IO.FileInfo]]::new()
    }
    $providers[$name].Add($binary)
}

$duplicateProviders = @($providers.GetEnumerator() |
    Where-Object { $_.Value.Count -gt 1 } |
    Sort-Object Key)
if ($duplicateProviders.Count -gt 0) {
    $details = foreach ($entry in $duplicateProviders) {
        $paths = $entry.Value | ForEach-Object { [IO.Path]::GetRelativePath($stage, $_.FullName) }
        "$($entry.Key): $($paths -join ', ')"
    }
    throw "The Windows payload has duplicate same-name DLL providers:`n$($details -join "`n")"
}

$importsByPath = @{}
foreach ($binary in $binaries) {
    $imports = @(& $dumpbin /nologo /dependents $binary.FullName |
        ForEach-Object {
            if ($_ -match '^\s+([^\s]+\.dll)\s*$') { $Matches[1] }
        } |
        Sort-Object -Unique)
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin could not inspect $($binary.FullName)"
    }
    $importsByPath[$binary.FullName.ToLowerInvariant()] = $imports
}

$rootPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in @('spool.exe', 'mpv-2.dll')) {
    $matches = @($binaries | Where-Object Name -EQ $name)
    if ($matches.Count -ne 1) {
        throw "The Windows runtime root must exist exactly once: $name"
    }
    [void] $rootPaths.Add($matches[0].FullName)
}
foreach ($directory in Get-ChildItem -LiteralPath $stage -Directory) {
    foreach ($plugin in Get-ChildItem -LiteralPath $directory.FullName -Recurse -File -Filter '*.dll') {
        [void] $rootPaths.Add($plugin.FullName)
    }
}
foreach ($relativeRoot in $RuntimeLoadedRoot) {
    $path = [IO.Path]::GetFullPath((Join-Path $stage $relativeRoot))
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "The explicitly runtime-loaded Windows root is missing: $relativeRoot"
    }
    [void] $rootPaths.Add($path)
}

$missing = [Collections.Generic.List[string]]::new()
$reachable = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$pending = [Collections.Generic.Queue[string]]::new()
foreach ($rootPath in $rootPaths) { $pending.Enqueue($rootPath) }
while ($pending.Count -gt 0) {
    $path = $pending.Dequeue()
    if (-not $reachable.Add($path)) { continue }
    $relative = [IO.Path]::GetRelativePath($stage, $path)
    foreach ($import in $importsByPath[$path.ToLowerInvariant()]) {
        $provider = $providers[$import.ToLowerInvariant()]
        if ($null -ne $provider) {
            $pending.Enqueue($provider[0].FullName)
            continue
        }
        if ($import -match '^(api|ext)-ms-win-') { continue }
        $isCompilerRuntime = $import -match '^(concrt|msvcp|vcruntime)\d*(_\d+)?\.dll$'
        $systemDll = Join-Path $env:SystemRoot "System32\$import"
        if (-not $isCompilerRuntime -and (Test-Path -LiteralPath $systemDll)) { continue }
        $missing.Add("$relative -> $import")
    }
}

$orphans = @($binaries |
    Where-Object { $_.Extension -eq '.dll' -and -not $reachable.Contains($_.FullName) } |
    Sort-Object FullName)
foreach ($orphan in $orphans) {
    Write-Output "ORPHAN`t$([IO.Path]::GetRelativePath($stage, $orphan.FullName))"
}

$inventory = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    [PSCustomObject]@{
        Path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/')
        Size = $_.Length
        Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
} | Sort-Object Path)
foreach ($item in $inventory) {
    Write-Output "$($item.Path)`tfile`t$($item.Size)`t$($item.Hash)"
}
$inventory | Group-Object Hash | Where-Object Count -GT 1 | Sort-Object Name | ForEach-Object {
    $paths = $_.Group.Path -join "`t"
    Write-Host "DUPLICATE`t$($_.Name)`t$paths"
}
$uniqueBytes = ($inventory | Group-Object Hash | ForEach-Object { $_.Group[0].Size } | Measure-Object -Sum).Sum
Write-Host "UNIQUE_REGULAR_BYTES`t$uniqueBytes"

if ($missing.Count -gt 0) {
    throw "The Windows payload has missing runtime dependencies:`n$($missing -join "`n")"
}
if ($orphans.Count -gt 0 -and -not $AllowOrphans) {
    throw "The Windows payload has $($orphans.Count) orphan runtime DLL(s)."
}
Write-Host "Runtime dependency closure passed for $($reachable.Count) Windows binaries."
