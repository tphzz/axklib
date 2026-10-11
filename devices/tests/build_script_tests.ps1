$ErrorActionPreference = 'Stop'
$BuildScript = Join-Path (Split-Path -Parent $PSScriptRoot) 'build.ps1'
if (-not (Test-Path -LiteralPath $BuildScript)) { throw 'build.ps1 is missing.' }

$Fixture = Join-Path ([System.IO.Path]::GetTempPath()) ('rawdisk-build-test-' + [Guid]::NewGuid().ToString('N'))
$EnvironmentNames = @('OS', 'VisualStudioVersion', 'VSCMD_ARG_TGT_ARCH', 'VSINSTALLDIR', 'WindowsSdkDir', 'WindowsSDKVersion')
$SavedEnvironment = @{}
foreach ($Name in $EnvironmentNames) {
    $SavedEnvironment[$Name] = [Environment]::GetEnvironmentVariable($Name)
}
$global:RawdiskBuildCalls = New-Object 'System.Collections.Generic.List[object]'
$global:RawdiskBuildFailure = ''
$DefaultRun = $null

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Throws([scriptblock]$Action) {
    $Threw = $false
    try { & $Action | Out-Null } catch { $Threw = $true }
    Assert-True $Threw 'Expected the build to fail.'
}

function New-Source([string]$Directory) {
    New-Item -ItemType Directory -Path $Directory -Force | Out-Null
    Copy-Item -LiteralPath $BuildScript -Destination (Join-Path $Directory 'build.ps1')
    [System.IO.File]::WriteAllText((Join-Path $Directory 'CMakeLists.txt'), '# fixture')
    return (Join-Path $Directory 'build.ps1')
}

function global:cmake {
    $Arguments = @($args)
    $global:RawdiskBuildCalls.Add([PSCustomObject]@{ Tool = 'cmake'; Arguments = $Arguments })
    $Stage = 'configure'
    if ($Arguments -contains '-P') { $Stage = 'architecture' }
    if ($Arguments[0] -eq '--build') {
        $Stage = 'build'
        foreach ($Name in @('rawtool.exe', 'axklib_helper.exe')) {
            [System.IO.File]::WriteAllText((Join-Path $Arguments[1] $Name), '# fixture')
        }
    }
    $global:LASTEXITCODE = 0
    if ($global:RawdiskBuildFailure -eq $Stage) { $global:LASTEXITCODE = 23 }
}

function global:ctest {
    $global:RawdiskBuildCalls.Add([PSCustomObject]@{ Tool = 'ctest'; Arguments = @($args) })
    $global:LASTEXITCODE = 0
    if ($global:RawdiskBuildFailure -eq 'tests') { $global:LASTEXITCODE = 23 }
}

try {
    $env:OS = 'Windows_NT'
    $env:VisualStudioVersion = '18.0'
    $env:VSCMD_ARG_TGT_ARCH = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    $env:VSINSTALLDIR = Join-Path $Fixture 'Visual Studio'
    $env:WindowsSdkDir = Join-Path $Fixture 'Windows SDK'
    $env:WindowsSDKVersion = '10.0.test.0'
    $Ninja = Join-Path $env:VSINSTALLDIR 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    $Header = Join-Path $env:WindowsSdkDir "Include/$env:WindowsSDKVersion/um/windows.h"
    foreach ($File in @($Ninja, $Header)) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $File) -Force | Out-Null
        [System.IO.File]::WriteAllText($File, '# fixture')
    }

    $Standalone = New-Source (Join-Path $Fixture 'standalone copy')
    $Run = Join-Path $Fixture 'standalone output'
    $Result = & $Standalone -RunDirectory $Run
    Assert-True ($global:RawdiskBuildCalls.Count -eq 3) 'Standalone build invoked an external repository check.'
    $Configure = $global:RawdiskBuildCalls[0].Arguments
    Assert-True ($Configure[$Configure.IndexOf('-S') + 1] -eq (Split-Path -Parent $Standalone)) 'Source must come from the script directory, not the working directory.'
    Assert-True ($Configure -contains '-DRAWDISK_WERROR=ON') 'Strict warnings were not enabled.'
    Assert-True ($Configure -contains '-DRAWDISK_BUILD_TESTS=ON') 'Tests were not enabled.'
    Assert-True ($Configure -contains '-DRAWDISK_BUILD_TOOLS=ON') 'Tools were not enabled.'
    Assert-True ($Result.RawTool -eq (Join-Path $Run 'native/rawtool.exe')) 'Wrong rawtool result path.'
    Assert-True (Test-Path -LiteralPath $Result.Helper) 'Helper was not built alongside rawtool.'

    $global:RawdiskBuildCalls.Clear()
    $Result = & $Standalone
    $DefaultRun = $Result.RunDirectory
    Assert-True (@($Result).Count -eq 1) 'Build output polluted the returned result.'
    Assert-True ($global:RawdiskBuildCalls.Count -eq 3) 'Default invocation did not build and test.'
    Assert-True ($DefaultRun -like (Join-Path ([System.IO.Path]::GetTempPath()) 'axklib-rawdisk-*')) 'Default output must use a fresh temporary directory.'
    Assert-True (Test-Path -LiteralPath $Result.RawTool) 'Default invocation did not return rawtool.'

    $global:RawdiskBuildCalls.Clear()
    $Repository = Join-Path $Fixture 'private repository'
    $InRepository = New-Source (Join-Path $Repository 'axklib/devices')
    $Checker = Join-Path $Repository 'axklib/cmake/ArchitectureCheck.cmake'
    New-Item -ItemType Directory -Path (Split-Path -Parent $Checker) -Force | Out-Null
    [System.IO.File]::WriteAllText($Checker, '# fixture')
    & $InRepository -RunDirectory (Join-Path $Fixture 'repository output') | Out-Null
    Assert-True ($global:RawdiskBuildCalls.Count -eq 4) 'Repository architecture check was omitted.'
    Assert-True ($global:RawdiskBuildCalls[0].Arguments -contains '-P') 'Architecture must run before configure/build/tests.'

    foreach ($Stage in @('architecture', 'configure', 'build', 'tests')) {
        $global:RawdiskBuildCalls.Clear()
        $global:RawdiskBuildFailure = $Stage
        $Script = $Standalone
        $ExpectedCalls = @{ architecture = 1; configure = 1; build = 2; tests = 3 }[$Stage]
        if ($Stage -eq 'architecture') { $Script = $InRepository }
        Assert-Throws { & $Script -RunDirectory (Join-Path $Fixture "failed-$Stage") }
        Assert-True ($global:RawdiskBuildCalls.Count -eq $ExpectedCalls) "Continued after a failed $Stage stage."
    }
    $global:RawdiskBuildFailure = ''

    $global:RawdiskBuildCalls.Clear()
    Assert-Throws { & $Standalone -RunDirectory $Run }
    Assert-True ($global:RawdiskBuildCalls.Count -eq 0) 'Reused a non-clean build directory.'

    Remove-Item -LiteralPath $Header
    Assert-Throws { & $Standalone -RunDirectory (Join-Path $Fixture 'missing SDK') }
    Assert-True ($global:RawdiskBuildCalls.Count -eq 0) 'Configured without the Windows SDK.'
    [System.IO.File]::WriteAllText($Header, '# fixture')

    $env:VSCMD_ARG_TGT_ARCH = 'invalid-target'
    Assert-Throws { & $Standalone -RunDirectory (Join-Path $Fixture 'wrong architecture') }
    Assert-True ($global:RawdiskBuildCalls.Count -eq 0) 'Configured with a non-native target.'
    Write-Host 'Build script orchestration: 10 cases passed (mock commands, not SDK qualification).'
} finally {
    foreach ($Name in $EnvironmentNames) {
        [Environment]::SetEnvironmentVariable($Name, $SavedEnvironment[$Name])
    }
    Remove-Item -LiteralPath $Fixture -Recurse -Force -ErrorAction SilentlyContinue
    if ($DefaultRun) { Remove-Item -LiteralPath $DefaultRun -Recurse -Force -ErrorAction SilentlyContinue }
    Remove-Item Function:global:cmake, Function:global:ctest
    Remove-Variable RawdiskBuildCalls, RawdiskBuildFailure -Scope Global
}
