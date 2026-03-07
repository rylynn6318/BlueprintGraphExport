param(
    [string]$EngineRoot = 'C:\UE5',
    [string]$RootPath = '/Engine/EngineSky'
)

$ErrorActionPreference = 'Stop'

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter()]
        [string[]]$ArgumentList = @(),
        [Parameter()]
        [int]$ExpectedExitCode = 0
    )

    & $FilePath @ArgumentList
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne $ExpectedExitCode) {
        throw "Native command failed with exit code ${ExitCode} (expected ${ExpectedExitCode}): $FilePath"
    }
}

function Normalize-PathValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\').Replace('\', '/')
}

$PluginRoot = Split-Path -Parent $PSScriptRoot
$PluginFile = Join-Path $PluginRoot 'BlueprintGraphExport.uplugin'
$PackageDir = Join-Path $PluginRoot 'Saved\VerifyPackage'
$HostDir = Join-Path $PluginRoot 'Saved\VerifyHost'
$HostProject = Join-Path $HostDir 'VerifyHost.uproject'
$HostPluginDir = Join-Path $HostDir 'Plugins\BlueprintGraphExport'
$VerifyOutputsDir = Join-Path $HostDir 'Saved\VerifyOutputs'
$ExpectedDocsRoot = Join-Path $VerifyOutputsDir 'Docs'
$ExpectedJsonRoot = Join-Path $VerifyOutputsDir 'Json'
$ExpectedSummaryPath = Join-Path $VerifyOutputsDir 'Summary.json'
$DefaultSummaryPath = Join-Path $HostDir 'Saved\BlueprintGraphExport\BlueprintGraphExportRunSummary.json'
$BuildVersionPath = Join-Path $EngineRoot 'Engine\Build\Build.version'
$EngineAssociation = '5.7'

if (Test-Path $BuildVersionPath) {
    $BuildVersion = Get-Content -Path $BuildVersionPath -Raw | ConvertFrom-Json
    if ($BuildVersion.MajorVersion -ne $null -and $BuildVersion.MinorVersion -ne $null) {
        $EngineAssociation = "$($BuildVersion.MajorVersion).$($BuildVersion.MinorVersion)"
    }
}

$UatArgs = @(
    'BuildPlugin',
    "-Plugin=$PluginFile",
    "-Package=$PackageDir"
)

Invoke-NativeCommand -FilePath (Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat') -ArgumentList $UatArgs

if (Test-Path $HostPluginDir) {
    Remove-Item -Path $HostPluginDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $HostPluginDir | Out-Null
Copy-Item "$PackageDir\*" $HostPluginDir -Recurse -Force

@"
{
  "FileVersion": 3,
  "EngineAssociation": "$EngineAssociation",
  "Category": "",
  "Description": "Temporary verification host project.",
  "Plugins": [
    {
      "Name": "BlueprintGraphExport",
      "Enabled": true
    }
  ]
}
"@ | Set-Content -Path $HostProject -Encoding UTF8

if (Test-Path $VerifyOutputsDir) {
    Remove-Item -Path $VerifyOutputsDir -Recurse -Force
}

$Args = @(
    $HostProject,
    '-run=BlueprintGraphExportSync',
    '-Unattended',
    '-NoSplash',
    '-NullRHI',
    '-Force',
    "-Roots=$RootPath",
    '-DocsRoot="Saved/VerifyOutputs/Docs"',
    '-JsonRoot="Saved/VerifyOutputs/Json"',
    '-ManifestPath="Saved/VerifyOutputs/Manifest.json"',
    '-SummaryPath="Saved/VerifyOutputs/Summary.json"'
)

Invoke-NativeCommand -FilePath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') -ArgumentList $Args

if (-not (Test-Path $ExpectedSummaryPath)) {
    throw "Summary output was not created: $ExpectedSummaryPath"
}

if (-not (Test-Path $ExpectedDocsRoot)) {
    throw "Docs output root was not created: $ExpectedDocsRoot"
}

$Summary = Get-Content -Path $ExpectedSummaryPath -Raw | ConvertFrom-Json
if ($Summary.status -ne 'success') {
    throw "Expected success summary status but found '$($Summary.status)'."
}

if ((Normalize-PathValue $Summary.documentation_root) -ne (Normalize-PathValue $ExpectedDocsRoot)) {
    throw "Summary documentation_root did not match expected path."
}

if ((Normalize-PathValue $Summary.json_root) -ne (Normalize-PathValue $ExpectedJsonRoot)) {
    throw "Summary json_root did not match expected path."
}

if ((Normalize-PathValue $Summary.summary_path) -ne (Normalize-PathValue $ExpectedSummaryPath)) {
    throw "Summary summary_path did not match expected path."
}

$MarkdownDoc = Get-ChildItem $ExpectedDocsRoot -Recurse -Filter *.md | Select-Object -First 1
if (-not $MarkdownDoc) {
    throw 'No markdown output was produced.'
}

if (Test-Path $DefaultSummaryPath) {
    Remove-Item -Path $DefaultSummaryPath -Force
}

$NegativeArgs = @(
    $HostProject,
    '-run=BlueprintGraphExportSync',
    '-Unattended',
    '-NoSplash',
    '-NullRHI',
    '-Roots=""'
)

Invoke-NativeCommand -FilePath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') -ArgumentList $NegativeArgs -ExpectedExitCode 2

if (-not (Test-Path $DefaultSummaryPath)) {
    throw "Default summary output was not created for invalid-arguments run: $DefaultSummaryPath"
}

$NegativeSummary = Get-Content -Path $DefaultSummaryPath -Raw | ConvertFrom-Json
if ($NegativeSummary.status -ne 'invalid_arguments') {
    throw "Expected invalid_arguments summary status but found '$($NegativeSummary.status)'."
}

Write-Output "Verification completed."
Write-Output "Summary: $ExpectedSummaryPath"
Write-Output "Markdown doc: $($MarkdownDoc.FullName)"
