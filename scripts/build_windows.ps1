[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$solution = Join-Path $repo 'm34thook.sln'
$stage = Join-Path $repo 'build/windows/Release-x64'
$rpcFiles = @(
    'RPCInterface/meathook_interface.h',
    'RPCInterface/meathook_interface_c.c',
    'RPCInterface/meathook_interface_s.c'
)

function Fail([string] $Message) {
    throw "build_windows.ps1: $Message"
}

if (-not (Test-Path -LiteralPath $solution -PathType Leaf)) { Fail "solution not found: $solution" }
foreach ($relative in $rpcFiles) {
    $path = Join-Path $repo $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item $path).Length -eq 0) {
        Fail "generated RPC artifact missing or empty: $relative; run scripts/generate_rpc.sh first"
    }
}
if ((Test-Path -LiteralPath $stage) -and -not (Test-Path -LiteralPath $stage -PathType Container)) {
    Fail "staging path is not a directory: $stage"
}

$vswhere = $null
$command = Get-Command vswhere.exe -ErrorAction SilentlyContinue
if ($command) { $vswhere = $command.Source }
if (-not $vswhere) {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/Installer/vswhere.exe')
    )
    $vswhere = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $vswhere) { Fail 'vswhere.exe not found; install Visual Studio Installer or add vswhere.exe to PATH' }

$instances = @((& $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.v142 -format json) | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or $instances.Count -eq 0) {
    Fail 'no Visual Studio instance with Microsoft.VisualStudio.Component.VC.v142 found'
}

$selected = $null
foreach ($instance in $instances) {
    $devCmd = Join-Path $instance.installationPath 'Common7/Tools/VsDevCmd.bat'
    $toolRoot = Join-Path $instance.installationPath 'VC/Tools/MSVC'
    $tool = Get-ChildItem -LiteralPath $toolRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^14\.2' } | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $tool -or -not (Test-Path (Join-Path $tool.FullName 'bin/Hostx64/x64/ml64.exe'))) { continue }
    if (-not (Test-Path -LiteralPath $devCmd -PathType Leaf)) { continue }

    $version = $tool.Name.Substring(0, 5)
    $dump = @(cmd.exe /d /s /c "`"$devCmd`" -arch=x64 -host_arch=x64 -vcvars_ver=$version && set")
    if ($LASTEXITCODE -ne 0) { continue }
    foreach ($line in $dump) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
    }

    $sdkLib = $null
    if ($env:WindowsSdkDir -and $env:WindowsSDKVersion) {
        $sdkLib = Join-Path (Join-Path $env:WindowsSdkDir 'Lib') $env:WindowsSDKVersion
    }
    $msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    $ml64 = Get-Command ml64.exe -ErrorAction SilentlyContinue
    if ($env:VCToolsInstallDir -match '\\14\.2' -and $sdkLib -and (Test-Path $sdkLib) -and $msbuild -and $ml64) {
        $selected = $instance
        break
    }
}
if (-not $selected) {
    Fail 'v142 C++ tools, Windows SDK, ml64.exe, and msbuild.exe are required; install VS16 v142 workload and Windows 10 SDK'
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('meathook-build-' + [Guid]::NewGuid().ToString('N'))
$libOut = Join-Path $work 'lib'
$libInt = Join-Path $work 'lib-obj'
$dllOut = Join-Path $work 'dll'
$dllInt = Join-Path $work 'dll-obj'
New-Item -ItemType Directory -Path $libOut, $libInt, $dllOut, $dllInt | Out-Null
try {
    $msbuildPath = (Get-Command msbuild.exe).Source
    $common = @('/m', '/nologo', '/t:Build', '/p:Configuration=Release', '/p:Platform=x64')
    & $msbuildPath (Join-Path $repo 'snaphak_algo/snaphak_algo.vcxproj') @common "/p:OutDir=$libOut\" "/p:IntDir=$libInt\"
    if ($LASTEXITCODE -ne 0) { Fail "snaphak_algo MSBuild failed with exit code $LASTEXITCODE" }
    & $msbuildPath (Join-Path $repo 'udis86test/udis86test.vcxproj') @common "/p:OutDir=$libOut\" "/p:IntDir=$libInt\"
    if ($LASTEXITCODE -ne 0) { Fail "udis86test MSBuild failed with exit code $LASTEXITCODE" }
    & $msbuildPath (Join-Path $repo 'm34thook/m34thook.vcxproj') @common "/p:OutDir=$dllOut\" "/p:IntDir=$dllInt\" "/p:AdditionalLibraryDirectories=$libOut"
    if ($LASTEXITCODE -ne 0) { Fail "MSBuild failed with exit code $LASTEXITCODE" }

    $built = Join-Path $dllOut 'XINPUT1_3.dll'
    if (-not (Test-Path -LiteralPath $built -PathType Leaf)) {
        Fail "expected build output not found in isolated output: $built"
    }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    $final = Join-Path $stage 'XINPUT1_3.dll'
    $temporary = "$final.$PID.tmp"
    Copy-Item -LiteralPath $built -Destination $temporary -Force
    Move-Item -LiteralPath $temporary -Destination $final -Force
    $hash = (Get-FileHash -LiteralPath $final -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output "XINPUT1_3.dll: $final"
    Write-Output "SHA256: $hash"
}
finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
