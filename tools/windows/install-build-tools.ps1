# Install the Windows build and packaging tools, for the Actions runner and a
# local checkout alike. The runner used to carry this list inline, so a local
# machine was set up by reading the workflow and retyping it -- and drifted
# from it. This script is the one copy; the workflow calls it.
#
#   tools\windows\install-build-tools.ps1            # install what is missing
#   tools\windows\install-build-tools.ps1 -VerifyOnly # only report
#
# MSYS2 supplies the shell FFmpeg is configured from. The runner installs it
# with msys2/setup-msys2 and points MSYS2_LOCATION at it, so this script only
# ensures its packages, and never installs MSYS2 itself.
param(
    [switch] $VerifyOnly
)

. (Join-Path $PSScriptRoot 'common.ps1')

$chocolateyPackages = @(
    @{ Name = 'meson'; Version = '1.9.1'; Provides = 'meson' },
    @{ Name = 'nasm'; Provides = 'nasm' },
    @{ Name = 'strawberryperl'; Provides = 'perl' },
    @{ Name = 'nsis'; Provides = 'makensis' },
    # The launch screen carries the version, so it is rendered at configure
    # time rather than kept in the tree, and ImageMagick draws it.
    @{ Name = 'imagemagick.tool'; Provides = 'magick' }
)

# Chocolatey installs to machine-wide locations, and these are where its
# packages put the tools that do not land on PATH by themselves.
$toolDirectories = @(
    (Join-Path $env:ProgramFiles 'LLVM\bin'),
    (Join-Path $env:ProgramFiles 'Meson'),
    (Join-Path $env:ProgramFiles 'NASM'),
    (Join-Path $env:SystemDrive 'Strawberry\perl\bin'),
    (Join-Path ${env:ProgramFiles(x86)} 'NSIS')
)

$requiredTools = @('cmake', 'ninja', 'clang', 'lld-link', 'meson', 'nasm', 'makensis', 'magick', 'perl')
$msys2Packages = @('make', 'diffutils', 'pkgconf', 'patch', 'tar', 'curl')

function Add-ToolDirectoriesToPath {
    foreach ($directory in $toolDirectories) {
        if ((Test-Path -LiteralPath $directory) -and ($env:PATH -notlike "*$directory*")) {
            $env:PATH = "$directory;$env:PATH"
        }
    }
}

Add-ToolDirectoriesToPath

if (-not $VerifyOnly) {
    if (-not (Get-Command choco.exe -ErrorAction SilentlyContinue)) {
        throw 'Chocolatey is required to install the Windows build tools. Install it from https://chocolatey.org/install, or install the tools yourself and re-run with -VerifyOnly.'
    }
    foreach ($package in $chocolateyPackages) {
        # A pinned package is installed even when something of that name is
        # already on PATH: the pin is the point, and the runner images carry
        # their own builds of some of these. Chocolatey is a no-op when the
        # pinned version is the installed one.
        $pinned = $package.ContainsKey('Version')
        $available = if ($package.Provides -eq 'perl') {
            [bool](Get-NativeWindowsPerl)
        } else { [bool](Get-Command "$($package.Provides).exe" -ErrorAction SilentlyContinue) }
        if (-not $pinned -and $available) {
            Write-Host "$($package.Name) is already installed"
            continue
        }
        $arguments = @('install', $package.Name, '-y', '--no-progress')
        if ($pinned) {
            $arguments += "--version=$($package.Version)"
        }
        Write-Host "Installing $($package.Name)"
        $installLog = & choco @arguments 2>&1
        $installExitCode = $LASTEXITCODE
        $installLog | Out-Host
        # A feed outage can leave a pinned package uninstalled even when
        # Chocolatey exits zero. Retry only a reported gateway/service error.
        $feedUnavailable = 'Failed to fetch results from V2 feed at ''https://community\.chocolatey\.org/api/v2/Packages.*(?:503|504)\b'
        if ($installLog -match $feedUnavailable) {
            Write-Warning "Chocolatey feed unavailable while installing $($package.Name). Retrying once."
            Start-Sleep -Seconds 10
            & choco @arguments
            $installExitCode = $LASTEXITCODE
        }
        if ($installExitCode -ne 0) {
            throw "Chocolatey failed to install $($package.Name) (exit $installExitCode). See the installation output above."
        }
        Add-ToolDirectoriesToPath
    }
}

$missing = @()
foreach ($tool in $requiredTools) {
    $command = Get-Command $tool -ErrorAction SilentlyContinue
    if ($tool -eq 'perl') {
        $nativePerl = Get-NativeWindowsPerl
        if ($nativePerl) {
            Write-Host "native Perl -> $nativePerl"
        } else { $missing += 'native Windows Perl (Strawberry Perl)' }
    } elseif ($command) {
        Write-Host "$tool -> $($command.Source)"
    } else {
        $missing += $tool
    }
}
if ($missing.Count -gt 0) {
    throw "Required build tools are missing: $($missing -join ', ')"
}

# Get-Msys2Root reports the installation and checks the tools FFmpeg needs, so
# a missing MSYS2 names itself here rather than midway through a libmpv build.
$msysRoot = Get-Msys2Root
Write-Host "msys2 -> $msysRoot"
if (-not $VerifyOnly -and -not $env:MSYS2_LOCATION) {
    # A local MSYS2 is installed without the packages; the runner's action
    # installs them itself, and its MSYS2_LOCATION is how that case is known.
    & (Join-Path $msysRoot 'usr\bin\bash.exe') -lc "pacman -S --noconfirm --needed $($msys2Packages -join ' ')"
    if ($LASTEXITCODE -ne 0) { throw 'Installing the MSYS2 packages failed.' }
}

Write-Host 'Windows build tools are ready.'
