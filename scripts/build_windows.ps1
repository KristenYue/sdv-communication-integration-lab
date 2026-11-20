$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Build Tools installer metadata was not found."
}
$installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installationPath) {
    throw "Visual C++ x64 build tools are required."
}
$developerPrompt = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
$runtimeDirectory = Join-Path $repositoryRoot ".runtime\windows-build"
New-Item -ItemType Directory -Force $runtimeDirectory | Out-Null

$common = '/nologo /std:c++20 /EHsc /W4 /permissive- /Z7 /Icpp\include cpp\src\vehicle_gateway.cpp cpp\src\fault_injector.cpp'
$command = @(
    ('call "{0}" -arch=x64' -f $developerPrompt),
    ('cl {0} cpp\tests\test_vehicle_gateway.cpp /Fe"{1}\sdv_core_tests.exe"' -f $common, $runtimeDirectory),
    ('"{0}\sdv_core_tests.exe"' -f $runtimeDirectory),
    ('cl {0} cpp\src\main.cpp /Fe"{1}\sdv_gateway_demo.exe"' -f $common, $runtimeDirectory),
    ('"{0}\sdv_gateway_demo.exe"' -f $runtimeDirectory),
    ('cl {0} cpp\src\gateway_cli.cpp /Fe"{1}\sdv_gateway_cli.exe"' -f $common, $runtimeDirectory)
) -join ' && '

Push-Location $repositoryRoot
try {
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Windows native build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

