param(
    [ValidateSet('Debug', 'Development', 'Release')]
    [string]$Configuration = 'Release',
    [string]$OutputName = 'reach_g0_20260921'
)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot -Parent
if ($OutputName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'OutputName must be a simple directory name.' }
$taskOutput = Join-Path $taskRepo "artifacts/$OutputName"
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskMsbuild = & $taskVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild/**/Bin/MSBuild.exe' | Select-Object -First 1
if (-not $taskMsbuild) { throw 'MSBuild not found.' }
$taskBuildArgs = @(
    (Join-Path $taskRepo 'TR2.sln'), '/nologo', '/m:1', '/nr:false', '/t:Build',
    "/p:Configuration=$Configuration", '/p:Platform=x64',
    "/p:ReachG0Root=$taskOutput",
    "/p:ForceImportBeforeCppTargets=$(Join-Path $PSScriptRoot 'reach_g0_paths.props')",
    '/verbosity:minimal',
    "/flp:logfile=$taskOutput/build_$Configuration.log;verbosity=normal;encoding=UTF-8"
)
$taskRecord = [ordered]@{
    started_at = (Get-Date).ToString('o')
    msbuild = $taskMsbuild
    arguments = $taskBuildArgs
    configuration = $Configuration
    output_root = $taskOutput
}
$taskRecord | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $taskOutput "build_${Configuration}_command.json")
# Python's Windows environment mapping normalizes variable names before creating
# the child process. This avoids duplicate Path/PATH entries from host launchers.
# No machine/user environment settings are changed or logged.
& python -c 'import os,subprocess,sys; sys.exit(subprocess.call(sys.argv[1:], env=dict(os.environ)))' $taskMsbuild @taskBuildArgs
$taskExit = $LASTEXITCODE
$taskRecord['finished_at'] = (Get-Date).ToString('o')
$taskRecord['exit_code'] = $taskExit
$taskExe = Join-Path $taskOutput "bin/$Configuration/GE3.exe"
if ($taskExit -eq 0 -and (Test-Path -LiteralPath $taskExe)) {
    $taskRecord['executable_sha256'] = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash
}
$taskRecord | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $taskOutput "build_${Configuration}_command.json")
exit $taskExit
