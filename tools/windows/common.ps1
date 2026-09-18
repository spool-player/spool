$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Import-MsvcEnvironment {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) {
            throw 'vswhere.exe was not found. Install Visual Studio 2022 with Desktop development with C++.'
        }

        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $installPath) {
            throw 'No Visual Studio installation with the x64 C++ toolchain was found.'
        }

        $vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
        $environment = & $env:ComSpec /d /s /c "`"$vcvars`" >nul && set"
        $importedPath = $null
        foreach ($line in $environment) {
            if ($line -match '^([^=]+)=(.*)$') {
                # Codex and some terminal hosts expose both PATH and Path. vcvars
                # updates PATH; prefer that spelling when both are present, but
                # GitHub runners expose only the conventional mixed-case Path.
                if ($Matches[1] -ieq 'PATH') {
                    if ($Matches[1] -ceq 'PATH' -or $null -eq $importedPath) {
                        $importedPath = $Matches[2]
                    }
                    continue
                }
                Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
            }
        }
        if ($null -ne $importedPath) {
            Set-Item -Path Env:PATH -Value $importedPath
        }
    }

    # GitHub's Windows image also exposes MinGW. Ninja otherwise discovers its
    # c++.exe before cl.exe and silently mixes the GNU ABI with MSVC Qt/QCoro.
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $env:CC = $compiler
    $env:CXX = $compiler
    foreach ($name in @('CC_LD', 'CXX_LD', 'WINDRES')) {
        Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
    }
}

function Get-RepositoryRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Get-Msys2Root {
    # CI supplies setup-msys2's actual installation; local installs use its usual path.
    $root = if ($env:MSYS2_LOCATION) { $env:MSYS2_LOCATION } else { 'C:\msys64' }
    foreach ($tool in @('bash.exe', 'cygpath.exe', 'make.exe', 'pkgconf.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root "usr\bin\$tool") -PathType Leaf)) {
            throw "Required MSYS2 tool is missing: $root\usr\bin\$tool. Install make, diffutils and pkgconf; set MSYS2_LOCATION for a non-default installation."
        }
    }
    return $root
}

function Expand-WindowsSourceArchive {
    param(
        [Parameter(Mandatory)] [string] $Archive,
        [Parameter(Mandatory)] [string] $Destination,
        [int] $StripComponents = 0
    )
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (-not (Test-Path -LiteralPath $tar)) {
        # Wine lacks inbox bsdtar, and MSYS tar's compression subprocess
        # cannot run reliably there. Python is already a build prerequisite.
        & python (Join-Path $PSScriptRoot 'extract-archive.py') `
            $Archive $Destination --strip-components $StripComponents
        if ($LASTEXITCODE -ne 0) { throw "Extracting $Archive failed." }
        return
    }
    & $tar -xf $Archive -C $Destination "--strip-components=$StripComponents"
    if ($LASTEXITCODE -ne 0) { throw "Extracting $Archive failed." }
}

# tools\manifests\toolchain.json is the single place the Qt and FFmpeg
# versions are set; nothing here should repeat one.
function Get-ToolchainManifest {
    $manifest = Join-Path (Get-RepositoryRoot) 'tools\manifests\toolchain.json'
    return Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
}

function Get-DefaultQtRoot {
    $qt = (Get-ToolchainManifest).qt
    return "C:\Qt\$($qt.version)\$($qt.windowsKit)"
}

function Get-DefaultQCoroRoot {
    return "C:\Qt\$((Get-ToolchainManifest).qcoro.windowsPrefix)"
}

# Include the working tree and build contract, not just the submodule commit:
# local edits must never reuse a DLL or disposable source mirror from before them.
function Get-MpvSourceRevision {
    $root = Get-RepositoryRoot
    $mpv = Join-Path $root 'mpv'
    $revision = & git -C $mpv rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $revision) {
        throw 'Cannot establish the libmpv source identity; a Git checkout is required.'
    }
    $files = & git -C $mpv -c core.quotePath=false ls-files --cached --others --exclude-standard
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate the libmpv working tree.' }
    $inputs = [Collections.Generic.List[string]]::new()
    $inputs.Add($revision.Trim())
    foreach ($file in ($files | Sort-Object -Unique)) {
        $path = Join-Path $mpv $file
        $hash = if (Test-Path -LiteralPath $path -PathType Leaf) {
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        } else { 'deleted' }
        $inputs.Add("$file=$hash")
    }
    foreach ($file in @(
        'tools/windows/build-mpv.ps1', 'tools/windows/common.ps1',
        'tools/windows/build-ffmpeg.ps1', 'tools/windows/build-ffmpeg.sh',
        'tools/windows/check-ffmpeg.py', 'tools/windows/build-shader-tools.ps1',
        'tools/windows/extract-archive.py',
        'tools/manifests/mpv-native.json', 'tools/manifests/ffmpeg-capabilities.json',
        'tools/manifests/toolchain.json', 'tools/ffmpeg-capabilities.py'
    )) {
        $inputs.Add("$file=$((Get-FileHash -LiteralPath (Join-Path $root $file) -Algorithm SHA256).Hash)")
    }
    foreach ($name in @('cl.exe', 'clang.exe', 'lld-link.exe')) {
        $tool = Get-Command $name -ErrorAction Stop
        $inputs.Add("$name=$((Get-FileHash -LiteralPath $tool.Source -Algorithm SHA256).Hash)")
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(
            ($inputs -join "`n")))).ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Get-MpvRevisionStampPath {
    param([Parameter(Mandatory)] [string] $Directory)
    return (Join-Path $Directory '.spool-mpv-revision')
}

function Read-MpvRevisionStamp {
    param([Parameter(Mandatory)] [string] $Directory)
    $stamp = Get-MpvRevisionStampPath -Directory $Directory
    if (-not (Test-Path -LiteralPath $stamp)) {
        return $null
    }
    return (Get-Content -LiteralPath $stamp -Raw).Trim()
}

function Write-MpvRevisionStamp {
    param(
        [Parameter(Mandatory)] [string] $Directory,
        [string] $Revision
    )
    if (-not $Revision) {
        return
    }
    [IO.File]::WriteAllText((Get-MpvRevisionStampPath -Directory $Directory), "$Revision`n",
        [Text.UTF8Encoding]::new($false))
}

# True when $Prefix holds a libmpv built from the submodule as it stands now.
# Missing source identity is an error, never permission to accept an old DLL.
function Test-MpvBuildCurrent {
    param([Parameter(Mandatory)] [string] $Prefix)
    if (-not (Test-Path -LiteralPath (Join-Path $Prefix 'lib\mpv.lib'))) {
        return $false
    }
    $revision = Get-MpvSourceRevision
    return ((Read-MpvRevisionStamp -Directory $Prefix) -eq $revision)
}

function Initialize-WindowsBuildEnvironment {
    Import-MsvcEnvironment

    $cmakeBin = Join-Path $env:ProgramFiles 'CMake\bin'
    if (Test-Path -LiteralPath $cmakeBin) {
        $env:PATH = "$cmakeBin;$env:PATH"
    }

    $qtRoot = if ($env:JELLYFIN_QT_ROOT) { $env:JELLYFIN_QT_ROOT } else { Get-DefaultQtRoot }
    $qcoroRoot = if ($env:JELLYFIN_QCORO_ROOT) { $env:JELLYFIN_QCORO_ROOT } else { Get-DefaultQCoroRoot }
    $mpvRoot = if ($env:JELLYFIN_MPV_ROOT) { $env:JELLYFIN_MPV_ROOT } else { Join-Path (Get-RepositoryRoot) 'build\windows-deps\mpv' }

    foreach ($path in @($qtRoot, $qcoroRoot)) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Required Windows dependency prefix was not found: $path"
        }
    }

    $env:JELLYFIN_QT_ROOT = $qtRoot
    $env:JELLYFIN_QCORO_ROOT = $qcoroRoot
    $env:JELLYFIN_MPV_ROOT = $mpvRoot
    $env:JELLYFIN_WINDOWS_PREFIX_PATH = "$qtRoot;$qcoroRoot"
    $env:PATH = "$qtRoot\bin;$env:PATH"
}

function Initialize-WindowsMpvBuildEnvironment {
    Import-MsvcEnvironment

    $toolDirectories = @(
        (Join-Path $env:ProgramFiles 'LLVM\bin'),
        (Join-Path $env:ProgramFiles 'Meson'),
        (Join-Path $env:ProgramFiles 'NASM'),
        (Join-Path $env:ProgramFiles 'Git\usr\bin')
    )
    foreach ($directory in $toolDirectories) {
        if (Test-Path -LiteralPath $directory) {
            $env:PATH = "$directory;$env:PATH"
        }
    }

    $requiredTools = @('clang.exe', 'clang++.exe', 'lld-link.exe', 'llvm-rc.exe', 'meson.exe', 'ninja.exe', 'nasm.exe')
    foreach ($tool in $requiredTools) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "Required Windows mpv build tool was not found: $tool"
        }
    }

    $env:CC = 'clang'
    $env:CXX = 'clang++'
    $env:CC_LD = 'lld-link'
    $env:CXX_LD = 'lld-link'
    $env:WINDRES = 'llvm-rc'
}

function Get-MpvFeatureArguments {
    param(
        [Parameter(Mandatory)] [string] $Platform,
        [switch] $IncludeSubprojects
    )

    $manifestPath = Join-Path (Get-RepositoryRoot) 'tools\manifests\mpv-native.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $platformArguments = $manifest.platforms.$Platform
    if ($null -eq $platformArguments) {
        throw "The mpv feature manifest has no platform named '$Platform'."
    }

    $arguments = @($manifest.common) + @($platformArguments)
    if ($IncludeSubprojects -and $manifest.subprojects.$Platform) {
        $arguments += @($manifest.subprojects.$Platform)
    }
    return @($arguments | ForEach-Object { "-D$_" })
}
