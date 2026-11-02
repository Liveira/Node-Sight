<#
.SYNOPSIS
Configure, compile, test and stage a native x64 Release build of Node-Sight.
.DESCRIPTION
Detects Visual Studio 2026/2022 if no generator is supplied. Generated files stay
in build/ and dist/; runtime preferences are never copied into packages.
.PARAMETER Launch
Open the staged widget after every required build/test/install step succeeds.
.PARAMETER Generator
Optional Visual Studio generator, for example 'Visual Studio 17 2022'.
#>
param([switch]$Launch, [string]$Generator)
$ErrorActionPreference = 'Stop'
$workspace = $PSScriptRoot
# Discover the installed C++ workload instead of assuming a developer's VS version.
if (-not $Generator) {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools with Desktop C++ is required.' }
    $version = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
    if ($version -match '^18\.') { $Generator = 'Visual Studio 18 2026' }
    elseif ($version -match '^17\.') { $Generator = 'Visual Studio 17 2022' }
    else { throw 'Install Visual Studio 2022 or 2026 with the Desktop C++ workload.' }
}
# Fail at the exact stage that fails; never package an old executable after an error.
cmake -S $workspace -B (Join-Path $workspace 'build') -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build (Join-Path $workspace 'build') --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
ctest --test-dir (Join-Path $workspace 'build') -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
cmake --install (Join-Path $workspace 'build') --config Release --prefix (Join-Path $workspace 'dist')
if ($LASTEXITCODE -ne 0) { throw 'Staging failed. Close the running widget and retry.' }
if ($Launch) { Start-Process -FilePath (Join-Path $workspace 'dist\NodeSight.exe') }
