# Build and install QCoro against the pinned Qt, for the Actions runner and a
# local checkout alike. The runner used to carry the clone, the CMake flags and
# the revision inline; this is the one copy of them.
#
#   tools\windows\install-qcoro.ps1          # build if the prefix is missing
#   tools\windows\install-qcoro.ps1 -Force   # rebuild over an existing prefix
#
# The revision and the prefix name come from tools\manifests\toolchain.json,
# which is the only place any toolchain version is named.
param(
    [string] $SourceDirectory,
    [switch] $Force,
    [string[]] $CMakeArguments = @()
)

. (Join-Path $PSScriptRoot 'common.ps1')

$manifest = (Get-ToolchainManifest).qcoro
$qtRoot = if ($env:JELLYFIN_QT_ROOT) { $env:JELLYFIN_QT_ROOT } else { Get-DefaultQtRoot }
$qcoroRoot = if ($env:JELLYFIN_QCORO_ROOT) { $env:JELLYFIN_QCORO_ROOT } else { Get-DefaultQCoroRoot }

if ((Test-Path -LiteralPath (Join-Path $qcoroRoot 'include')) -and -not $Force) {
    Write-Host "QCoro $($manifest.version) is already installed at $qcoroRoot"
    return
}

if (-not (Test-Path -LiteralPath $qtRoot)) {
    throw "Qt was not found at $qtRoot. Run tools\windows\install-qt.ps1 first."
}

Import-MsvcEnvironment

$scratch = if ($SourceDirectory) {
    [IO.Path]::GetFullPath($SourceDirectory)
} elseif ($env:RUNNER_TEMP) {
    Join-Path $env:RUNNER_TEMP 'qcoro'
} else {
    Join-Path (Get-RepositoryRoot) 'build\windows-deps\qcoro-source'
}
$build = "$scratch-build"

if (-not (Test-Path -LiteralPath (Join-Path $scratch '.git'))) {
    if (Test-Path -LiteralPath $scratch) {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
    git clone --filter=blob:none --no-checkout https://github.com/danvratil/qcoro.git $scratch
    if ($LASTEXITCODE -ne 0) { throw 'Cloning QCoro failed.' }
}
git -C $scratch fetch --depth 1 origin $manifest.revision
if ($LASTEXITCODE -ne 0) { throw "Fetching QCoro $($manifest.revision) failed." }
git -C $scratch checkout --detach FETCH_HEAD
if ($LASTEXITCODE -ne 0) { throw 'Checking out the pinned QCoro revision failed.' }

# Only what the app links against: the coroutine wrappers for Core and Network.
cmake -S $scratch -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_PREFIX_PATH=$qtRoot" `
    "-DCMAKE_INSTALL_PREFIX=$qcoroRoot" `
    -DQCORO_BUILD_EXAMPLES=OFF `
    -DQCORO_BUILD_TESTING=OFF `
    -DQCORO_WITH_QTDBUS=OFF `
    -DQCORO_WITH_QML=OFF `
    -DQCORO_WITH_QTQUICK=OFF `
    -DQCORO_WITH_QTTEST=OFF `
    -DQCORO_WITH_QTWEBSOCKETS=OFF @CMakeArguments
if ($LASTEXITCODE -ne 0) { throw 'QCoro configuration failed.' }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'QCoro build failed.' }
cmake --install $build
if ($LASTEXITCODE -ne 0) { throw 'QCoro installation failed.' }

Write-Host "Installed QCoro $($manifest.version) at $qcoroRoot"
