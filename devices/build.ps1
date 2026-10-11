[CmdletBinding()]
param(
    [string]$RunDirectory
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if ($env:OS -ne 'Windows_NT') { throw 'This build script requires Windows.' }
Get-Command cmake, ctest -ErrorAction Stop | Out-Null

$Source = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $Source 'CMakeLists.txt'))) {
    throw 'Keep build.ps1 in the complete rawdisk directory.'
}

$Repository = Split-Path -Parent (Split-Path -Parent $Source)
$AxkSource = Join-Path $Repository 'axklib'
$ArchitectureCheck = Join-Path $AxkSource 'cmake/ArchitectureCheck.cmake'
if (Test-Path -LiteralPath $ArchitectureCheck) {
    & cmake "-DAXK_SOURCE_ROOT=$AxkSource" -P $ArchitectureCheck | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Architecture check failed (exit $LASTEXITCODE)." }
} else {
    Write-Host 'Standalone rawdisk: the parent architecture check is not available.'
}

$HostArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($HostArch -notin @('x64', 'arm64') -or
    $env:VisualStudioVersion -notlike '18.*' -or $env:VSCMD_ARG_TGT_ARCH -ne $HostArch) {
    throw 'Use a non-elevated native Developer PowerShell for Visual Studio 2026 (x64 or ARM64).'
}
if (-not $env:VSINSTALLDIR -or -not $env:WindowsSdkDir -or -not $env:WindowsSDKVersion) {
    throw 'Visual Studio and Windows SDK environment variables are missing.'
}
$SdkHeader = Join-Path $env:WindowsSdkDir "Include/$env:WindowsSDKVersion/um/windows.h"
if (-not (Test-Path -LiteralPath $SdkHeader)) { throw 'The selected Windows SDK is missing windows.h.' }
$Ninja = Join-Path $env:VSINSTALLDIR 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (-not (Test-Path -LiteralPath $Ninja)) { throw 'Visual Studio CMake/Ninja tools are missing.' }

if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $Name = 'axklib-rawdisk-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $RunDirectory = Join-Path ([System.IO.Path]::GetTempPath()) $Name
}
New-Item -ItemType Directory -Path $RunDirectory -Force | Out-Null
$RunDirectory = (Resolve-Path -LiteralPath $RunDirectory).Path
$Build = Join-Path $RunDirectory 'native'
if (Test-Path -LiteralPath $Build) { throw "Build directory already exists; choose a fresh RunDirectory: $Build" }
New-Item -ItemType Directory -Path $Build | Out-Null

& cmake -S $Source -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" -DCMAKE_CXX_COMPILER=cl `
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
    -DRAWDISK_WERROR=ON -DRAWDISK_BUILD_TESTS=ON -DRAWDISK_BUILD_TOOLS=ON | Out-Host
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

& cmake --build $Build --parallel 2 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }

& ctest --test-dir $Build --output-on-failure --timeout 120 --no-tests=error | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Tests failed (exit $LASTEXITCODE)." }

$Raw = Join-Path $Build 'rawtool.exe'
$Helper = Join-Path $Build 'axklib_helper.exe'
foreach ($Executable in @($Raw, $Helper)) {
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Build output is missing: $Executable" }
}
Write-Host "Build and tests passed. rawtool: $Raw"
Write-Host "Elevation helper: $Helper"
Write-Host 'No physical device was opened or written by this script.'
[PSCustomObject]@{
    RunDirectory = $RunDirectory
    BuildDirectory = $Build
    RawTool = $Raw
    Helper = $Helper
}
