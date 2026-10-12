[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
  [int]$Parallel = 4,
  [string]$CMakePath
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $CMakePath) {
  $taskCommand = Get-Command cmake -ErrorAction SilentlyContinue
  if ($taskCommand) { $CMakePath = $taskCommand.Source }
  else {
    $taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $taskVsWhere) {
      $taskVs = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($taskVs) { $CMakePath = Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' }
    }
  }
}
if (-not $CMakePath -or -not (Test-Path -LiteralPath $CMakePath)) { throw 'CMake not found. Install CMake or the Visual Studio CMake component.' }
$taskCTest = Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe'
$taskBuild = Join-Path $taskRoot 'build'
& $CMakePath -S $taskRoot -B $taskBuild -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
& $CMakePath --build $taskBuild --config $Configuration --target mini_daw mini_daw_routing_test --parallel $Parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
& $taskCTest --test-dir $taskBuild -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed.' }
$taskOutput = Join-Path $taskRoot 'out'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskReady = Join-Path $taskOutput 'MiniDAW.exe'
try {
  Copy-Item -LiteralPath (Join-Path $taskBuild "mini_daw_artefacts/$Configuration/MiniDAW.exe") -Destination $taskReady
} catch {
  if (($_.Exception.HResult -band 0xffff) -notin @(32, 33)) { throw }
  $taskReady = Join-Path $taskOutput 'MiniDAW-next.exe'
  Copy-Item -LiteralPath (Join-Path $taskBuild "mini_daw_artefacts/$Configuration/MiniDAW.exe") -Destination $taskReady
  Write-Warning 'The current mixer is running. The rebuilt executable is saved as MiniDAW-next.exe; close the current mixer before replacing MiniDAW.exe.'
}
Write-Host "Ready: $taskReady"
