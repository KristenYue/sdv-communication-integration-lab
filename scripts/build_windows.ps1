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

$environmentLines = & cmd.exe /d /s /c ('call "{0}" -arch=x64 >nul && set' -f $developerPrompt)
if ($LASTEXITCODE -ne 0) {
    throw "Visual Studio developer environment initialization failed."
}
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}

$common = @(
    '/nologo', '/std:c++20', '/EHsc', '/W4', '/permissive-', '/Z7', '/Icpp\include',
    'cpp\src\vehicle_gateway.cpp', 'cpp\src\fault_injector.cpp',
    'cpp\src\vehicle_service_runtime.cpp', 'cpp\src\idempotency_journal.cpp',
    'cpp\src\someip.cpp', 'cpp\src\someip_udp.cpp',
    'cpp\src\vehicle_someip_adapter.cpp', 'ws2_32.lib', 'psapi.lib'
)

function Invoke-NativeBuild {
    param(
        [Parameter(Mandatory)][string[]]$Sources,
        [Parameter(Mandatory)][string]$Output,
        [string[]]$Options = @(),
        [switch]$Run
    )
    & cl.exe @common @Options @Sources "/Fe$Output"
    if ($LASTEXITCODE -ne 0) {
        throw "Native compile failed for $Output with exit code $LASTEXITCODE."
    }
    if ($Run) {
        & $Output
        if ($LASTEXITCODE -ne 0) {
            throw "Native executable failed for $Output with exit code $LASTEXITCODE."
        }
    }
}

Push-Location $repositoryRoot
try {
    Invoke-NativeBuild -Sources @('cpp\tests\test_vehicle_gateway.cpp') `
        -Output "$runtimeDirectory\sdv_core_tests.exe" -Run
    Invoke-NativeBuild `
        -Sources @('cpp\src\diagnostic_manager.cpp', 'cpp\src\vehicle_diagnostic_service.cpp',
                   'cpp\tests\test_vehicle_service_runtime.cpp') `
        -Output "$runtimeDirectory\sdv_service_runtime_tests.exe" -Run
    Invoke-NativeBuild -Sources @('cpp\tests\test_someip.cpp') `
        -Output "$runtimeDirectory\sdv_someip_tests.exe" -Run
    Invoke-NativeBuild -Sources @('cpp\tests\test_vehicle_someip_adapter.cpp') `
        -Output "$runtimeDirectory\sdv_vehicle_someip_adapter_tests.exe" -Run
    Invoke-NativeBuild `
        -Sources @('cpp\src\diagnostic_manager.cpp', 'cpp\tests\test_diagnostic_manager.cpp') `
        -Output "$runtimeDirectory\sdv_diagnostic_manager_tests.exe" -Run
    Invoke-NativeBuild -Sources @('cpp\src\main.cpp') `
        -Output "$runtimeDirectory\sdv_gateway_demo.exe" -Run
    Invoke-NativeBuild -Sources @('cpp\src\service_runtime_demo.cpp') `
        -Output "$runtimeDirectory\sdv_service_runtime_demo.exe" -Run
    $targets = @(
        @('cpp\src\gateway_cli.cpp', 'sdv_gateway_cli.exe'),
        @('cpp\src\someip_vehicle_server.cpp', 'sdv_someip_vehicle_server.exe'),
        @('cpp\src\someip_vehicle_client.cpp', 'sdv_someip_vehicle_client.exe'),
        @('cpp\src\someip_body_client.cpp', 'sdv_someip_body_client.exe'),
        @('cpp\src\idempotency_evidence.cpp', 'sdv_idempotency_evidence.exe'),
        @('cpp\src\restart_recovery_evidence.cpp', 'sdv_restart_recovery_evidence.exe'),
        @('cpp\src\concurrency_evidence.cpp', 'sdv_concurrency_evidence.exe')
    )
    foreach ($target in $targets) {
        Invoke-NativeBuild -Sources @($target[0]) -Output "$runtimeDirectory\$($target[1])"
    }
    Invoke-NativeBuild -Sources @('cpp\src\performance_matrix.cpp') `
        -Output "$runtimeDirectory\sdv_performance_matrix.exe" -Options @('/O2')
    Invoke-NativeBuild `
        -Sources @('cpp\src\diagnostic_manager.cpp', 'cpp\src\diagnostic_evidence.cpp') `
        -Output "$runtimeDirectory\sdv_diagnostic_evidence.exe"
    Invoke-NativeBuild `
        -Sources @('cpp\src\diagnostic_manager.cpp', 'cpp\src\vehicle_diagnostic_service.cpp',
                   'cpp\src\diagnostic_service_evidence.cpp') `
        -Output "$runtimeDirectory\sdv_diagnostic_service_evidence.exe"
} finally {
    Pop-Location
}

