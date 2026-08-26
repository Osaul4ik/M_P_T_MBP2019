<#
    build-gui.ps1
    Publishes only AmtPtpConfigGui (Wellspring Control Center) for x64.
    The driver (AmtPtpDeviceUsbKm) is NOT touched.

    Requirements on the build machine:
      - Windows
      - .NET 8 SDK

    Run from anywhere; the script automatically finds the repository root
    of wellspring-ptp, branch Dev, relative to its own location:
    src/AmtPtpConfigGui/installer/build-gui.ps1

      pwsh .\build-gui.ps1
      # or
      powershell -ExecutionPolicy Bypass -File .\build-gui.ps1
#>

param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..\..\.."),
    [string]$Configuration = "Release",
    [string]$Runtime = "win-x64",
    [string]$OutDir = "$PSScriptRoot\publish\$Runtime"
)

$ErrorActionPreference = "Stop"

$csproj = Join-Path $RepoRoot "src\AmtPtpConfigGui\AmtPtpConfigGui.csproj"
if (-not (Test-Path $csproj)) {
    throw "Could not find $csproj. Pass -RepoRoot <path to the wellspring-ptp repository root>."
}

if (Test-Path $OutDir) {
    Remove-Item $OutDir -Recurse -Force
}

Write-Host "== Publish AmtPtpConfigGui ($Runtime, $Configuration) ==" -ForegroundColor Cyan

dotnet publish $csproj `
    -c $Configuration `
    -r $Runtime `
    --self-contained false `
    -p:Platform=x64 `
    -p:PublishSingleFile=false `
    -o $OutDir

if ($LASTEXITCODE -ne 0) {
    throw "dotnet publish failed ($LASTEXITCODE)"
}

Write-Host "OK -> $OutDir" -ForegroundColor Green
Write-Host "Next: compile WellspringControlCenter.iss (in this same folder) with Inno Setup Compiler." -ForegroundColor Yellow
