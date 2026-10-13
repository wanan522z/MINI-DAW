[CmdletBinding()]
param(
    [string]$AsioProject,
    [string]$Compiler,
    [string]$RuntimeInstaller,
    [string]$ApplicationExe,
    [string]$CMakePath,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $AsioProject) { $AsioProject = Join-Path (Split-Path -Parent $taskRoot) 'asio' }
$AsioProject = (Resolve-Path -LiteralPath $AsioProject).Path
$taskDriverManifest = Get-Content -LiteralPath (Join-Path $AsioProject 'dist\package-manifest.json') -Raw | ConvertFrom-Json
if ($taskDriverManifest.Architecture -ne 'x64' -or $taskDriverManifest.Version -notmatch '^v[0-9]+$') {
    throw 'The Aggregate Audio project must provide a packaged x64 release and its manifest.'
}
$taskDriverVersion = $taskDriverManifest.Version
$taskDriverInstaller = Join-Path $AsioProject "dist\AggregateAudio-$taskDriverVersion-x64-Setup.exe"
if ((Get-FileHash -LiteralPath $taskDriverInstaller -Algorithm SHA256).Hash -ne $taskDriverManifest.InstallerSHA256) {
    throw 'The Aggregate Audio installer does not match its release manifest.'
}
if (-not $Compiler) { $Compiler = Join-Path $AsioProject 'build\packaging-tools\inno-7.1.0\tools\ISCC.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$taskCompilerSignature = Get-AuthenticodeSignature -LiteralPath $Compiler
if ($taskCompilerSignature.Status -ne 'Valid' -or $taskCompilerSignature.SignerCertificate.Subject -notmatch 'Pyrsys B.V.') {
    throw 'Use the authentic, signed Inno Setup compiler.'
}
$taskCompilerVersion = (& $Compiler '--version' | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $taskCompilerVersion -ne '7.1.0') { throw 'This package uses Inno Setup 7.1.0.' }
if (-not $RuntimeInstaller) {
    $RuntimeInstaller = Join-Path $AsioProject "build\installer-$taskDriverVersion\payload\vc_redist.x64.exe"
    if ((Get-FileHash -LiteralPath $RuntimeInstaller -Algorithm SHA256).Hash -ne $taskDriverManifest.RuntimeSHA256) {
        throw 'The embedded runtime does not match the Aggregate Audio release manifest.'
    }
}
$RuntimeInstaller = (Resolve-Path -LiteralPath $RuntimeInstaller).Path
$taskRuntimeSignature = Get-AuthenticodeSignature -LiteralPath $RuntimeInstaller
if ($taskRuntimeSignature.Status -ne 'Valid' -or $taskRuntimeSignature.SignerCertificate.Subject -notmatch 'CN=Microsoft Corporation,') {
    throw 'The x64 runtime installer must have a valid Microsoft signature.'
}
if ([version](Get-Item -LiteralPath $RuntimeInstaller).VersionInfo.FileVersion -lt [version]'14.44.35112.0') {
    throw 'The Visual C++ x64 runtime installer is too old.'
}

if (-not $ApplicationExe) {
    if (-not $SkipBuild) {
        if (-not $CMakePath) {
            $taskCommand = Get-Command cmake -ErrorAction SilentlyContinue
            if ($taskCommand) { $CMakePath = $taskCommand.Source }
            else {
                $taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
                $taskVsPath = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
                if ($taskVsPath) { $CMakePath = Join-Path $taskVsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
            }
        }
        if (-not $CMakePath -or -not (Test-Path -LiteralPath $CMakePath)) { throw 'CMake was not found.' }
        $taskBuild = Join-Path $taskRoot 'build'
        if (-not (Test-Path -LiteralPath (Join-Path $taskBuild 'CMakeCache.txt'))) {
            & $CMakePath -S $taskRoot -B $taskBuild -G 'Visual Studio 17 2022' -A x64
            if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
        }
        & $CMakePath --build $taskBuild --config Release --target mini_daw --parallel 4
        if ($LASTEXITCODE -ne 0) { throw 'Mini DAW build failed.' }
    }
    $ApplicationExe = Join-Path $taskRoot 'build\mini_daw_artefacts\Release\MiniDAW.exe'
}
$ApplicationExe = (Resolve-Path -LiteralPath $ApplicationExe).Path
$taskCMakeSource = Get-Content -LiteralPath (Join-Path $taskRoot 'CMakeLists.txt') -Raw
$taskVersionMatch = [regex]::Match($taskCMakeSource, 'project\(minidaw\s+VERSION\s+(\d+\.\d+\.\d+)')
if (-not $taskVersionMatch.Success) { throw 'Mini DAW version was not found in CMakeLists.txt.' }
$taskVersion = $taskVersionMatch.Groups[1].Value

function Assert-X64Executable([string]$Path) {
    $taskReader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($taskReader.ReadUInt16() -ne 0x5A4D) { throw "Invalid PE file: $Path" }
        $taskReader.BaseStream.Position = 0x3C
        $taskPeOffset = $taskReader.ReadUInt32()
        $taskReader.BaseStream.Position = $taskPeOffset
        if ($taskReader.ReadUInt32() -ne 0x00004550 -or $taskReader.ReadUInt16() -ne 0x8664) {
            throw "Expected an x64 executable: $Path"
        }
    } finally { $taskReader.Dispose() }
}
Assert-X64Executable $ApplicationExe

$taskPayload = Join-Path $taskRoot 'build\installer\payload'
$taskLicenseDirectory = Join-Path $taskPayload 'licenses'
$taskOutput = Join-Path $taskRoot 'dist'
New-Item -ItemType Directory -Path $taskPayload, $taskLicenseDirectory, $taskOutput -Force | Out-Null
$taskSources = [ordered]@{
    'MiniDAW.exe' = $ApplicationExe
    'AggregateAudio-Setup.exe' = $taskDriverInstaller
    'vc_redist.x64.exe' = $RuntimeInstaller
}
$taskHashes = [ordered]@{}
foreach ($taskName in $taskSources.Keys) {
    $taskSource = $taskSources[$taskName]
    $taskDestination = Join-Path $taskPayload $taskName
    $taskSourceHash = (Get-FileHash -LiteralPath $taskSource -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $taskSource -Destination $taskDestination -Force
    if ((Get-FileHash -LiteralPath $taskDestination -Algorithm SHA256).Hash -ne $taskSourceHash) { throw "Payload copy mismatch: $taskName" }
    $taskHashes[$taskName] = $taskSourceHash
}
$taskHashLines = foreach ($taskName in $taskHashes.Keys) { "$($taskHashes[$taskName])  $taskName" }
$taskHashLines | Set-Content -LiteralPath (Join-Path $taskPayload 'binaries.sha256') -Encoding ascii

$taskJuce = Join-Path $taskRoot 'third_party\JUCE'
if (-not (Test-Path -LiteralPath $taskJuce)) {
    $taskJuce = Join-Path $taskRoot 'build\_deps\juce-src'
}
$taskLicenceSources = [ordered]@{
    'JUCE-LICENSE.md' = (Join-Path $taskJuce 'LICENSE.md')
    'Steinberg-ASIO-SDK.txt' = (Join-Path $taskJuce 'modules\juce_audio_devices\native\asio\LICENSE.txt')
    'Steinberg-VST3-SDK.txt' = (Join-Path $taskJuce 'modules\juce_audio_processors_headless\format_types\VST3_SDK\LICENSE.txt')
    'FLAC.txt' = (Join-Path $taskJuce 'modules\juce_audio_formats\codecs\flac\Flac Licence.txt')
    'Ogg-Vorbis.txt' = (Join-Path $taskJuce 'modules\juce_audio_formats\codecs\oggvorbis\Ogg Vorbis Licence.txt')
    'PNG.txt' = (Join-Path $taskJuce 'modules\juce_graphics\image_formats\pnglib\LICENSE')
    'JPEG.txt' = (Join-Path $taskJuce 'modules\juce_graphics\image_formats\jpglib\README')
    'zlib.txt' = (Join-Path $taskJuce 'modules\juce_core\zip\zlib\LICENSE')
    'HarfBuzz.txt' = (Join-Path $taskJuce 'modules\juce_graphics\fonts\harfbuzz\COPYING')
    'SheenBidi.txt' = (Join-Path $taskJuce 'modules\juce_graphics\unicode\sheenbidi\LICENSE')
    'Inno-Setup-LICENSE.txt' = (Join-Path (Split-Path -Parent $Compiler) 'license.txt')
}
foreach ($taskName in $taskLicenceSources.Keys) {
    Copy-Item -LiteralPath $taskLicenceSources[$taskName] -Destination (Join-Path $taskLicenseDirectory $taskName) -Force
}

& $Compiler '--quiet' "--define=AppVersion=$taskVersion" "--define=AggregateVersion=$taskDriverVersion" `
    "--define=PayloadDir=$taskPayload" "--define=OutputDir=$taskOutput" (Join-Path $PSScriptRoot 'MiniDAW.iss')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed: $LASTEXITCODE" }
$taskInstaller = Join-Path $taskOutput "MiniDAW-$taskVersion-x64-Setup.exe"
$taskInstallerHash = (Get-FileHash -LiteralPath $taskInstaller -Algorithm SHA256).Hash
"$taskInstallerHash  $([IO.Path]::GetFileName($taskInstaller))" |
    Set-Content -LiteralPath (Join-Path $taskOutput "MiniDAW-$taskVersion-x64-Setup.sha256") -Encoding ascii
[ordered]@{
    Version = $taskVersion
    Architecture = 'x64'
    Installer = $taskInstaller
    InstallerSHA256 = $taskInstallerHash
    InstallerBytes = (Get-Item -LiteralPath $taskInstaller).Length
    AggregateAudioVersion = $taskDriverVersion
    Payload = $taskHashes
    RuntimeVersion = (Get-Item -LiteralPath $RuntimeInstaller).VersionInfo.FileVersion
    CompilerVersion = $taskCompilerVersion
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskOutput 'package-manifest.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'INSTALL-zh-CN.txt') -Destination $taskOutput -Force
Get-Item -LiteralPath $taskInstaller | Select-Object FullName, Length
Write-Output "SHA256: $taskInstallerHash"
