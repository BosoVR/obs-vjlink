param(
    [string]$ObsDir = "F:\obs-studio",
    [switch]$AlsoDeploySdk
)

$ErrorActionPreference = "Stop"

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repo

$obsProcesses = Get-Process -Name obs64,obs32,obs -ErrorAction SilentlyContinue
if ($obsProcesses) {
    $names = ($obsProcesses | ForEach-Object { "$($_.ProcessName)#$($_.Id)" }) -join ", "
    throw "OBS is running ($names). Close OBS before deploying obs-vjlink.dll."
}

$vsRoot = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$msvcToolsRoot = Join-Path $vsRoot "VC\Tools\MSVC"
$msvcDir = Get-ChildItem -LiteralPath $msvcToolsRoot -Directory |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (!$msvcDir) {
    throw "MSVC tools not found below: $msvcToolsRoot"
}

$clExe = Join-Path $msvcDir.FullName "bin\Hostx64\x64\cl.exe"
$linkExe = Join-Path $msvcDir.FullName "bin\Hostx64\x64\link.exe"
if (!(Test-Path -LiteralPath $clExe) -or !(Test-Path -LiteralPath $linkExe)) {
    throw "MSVC cl/link not found below: $($msvcDir.FullName)"
}

$kitsRoot = "C:\Program Files (x86)\Windows Kits\10"
$sdkIncludeRoot = Join-Path $kitsRoot "Include"
$sdkLibRoot = Join-Path $kitsRoot "Lib"
$sdkDir = Get-ChildItem -LiteralPath $sdkIncludeRoot -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "ucrt") } |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (!$sdkDir) {
    throw "Windows SDK include directory not found below: $sdkIncludeRoot"
}
$sdkVersion = $sdkDir.Name

$manual = Join-Path $repo "build\manual-release"
New-Item -ItemType Directory -Force -Path $manual | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $repo "build\Release") | Out-Null

$sources = @(
    "src\plugin-main.c",
    "src\vjlink_context.c",
    "src\state\vjlink_state.c",
    "src\audio\audio_engine.c",
    "src\audio\audio_texture.c",
    "src\audio\bpm_detector.c",
    "src\audio\kissfft\kiss_fft.c",
    "src\rendering\effect_system.c",
    "src\rendering\compositor.c",
    "src\rendering\engine3d.c",
    "src\rendering\particles.c",
    "src\rendering\band_effects.c",
    "src\rendering\media_layer.c",
    "src\sources\audio_filter.c",
    "src\sources\compositor_source.c",
    "src\sources\effect_filter.c",
    "src\sources\videowall_source.c",
    "src\controls\hotkey_manager.c",
    "src\controls\websocket_handler.c",
    "src\controls\source_trigger.c",
    "src\controls\http_server.c",
    "src\controls\tools_menu.c",
    "src\controls\osc_sender.c",
    "src\presets\preset_manager.c",
    "src\presets\param_animator.c",
    "src\presets\cjson\cJSON.c",
    "src\ui\properties_builder.c"
)

$obsSdk = Join-Path (Split-Path $repo -Parent) "obs-sdk"
$obsInclude = Join-Path $obsSdk "include"
$obsIncludeObs = Join-Path $obsSdk "include\obs"
$obsThreads = Join-Path $obsSdk "deps\w32-pthreads"
$obsLib = Join-Path $obsSdk "lib"

$compileRsp = Join-Path $manual "compile.rsp"
$linkRsp = Join-Path $manual "link.rsp"

$compileArgs = @(
    "/c",
    "/nologo",
    "/W1",
    "/WX-",
    "/diagnostics:column",
    "/O2",
    "/Ob2",
    "/MD",
    "/std:c11",
    "/D_WINDLL",
    "/D_MBCS",
    "/DWIN32",
    "/D_WINDOWS",
    "/DNDEBUG",
    "/D_CRT_SECURE_NO_WARNINGS",
    "/DWIN32_LEAN_AND_MEAN",
    "/D_USE_MATH_DEFINES",
    "/DCJSON_HIDE_SYMBOLS",
    "/Dobs_vjlink_EXPORTS",
    "/DCMAKE_INTDIR=""Release""",
    "/I""$obsInclude""",
    "/I""$obsIncludeObs""",
    "/I""$obsThreads""",
    "/I""$repo\src""",
    "/I""$repo\src\audio\kissfft""",
    "/I""$($msvcDir.FullName)\include""",
    "/I""$kitsRoot\Include\$sdkVersion\ucrt""",
    "/I""$kitsRoot\Include\$sdkVersion\shared""",
    "/I""$kitsRoot\Include\$sdkVersion\um""",
    "/I""$kitsRoot\Include\$sdkVersion\winrt""",
    "/I""$kitsRoot\Include\$sdkVersion\cppwinrt""",
    "/Fobuild\manual-release\",
    "/Fdbuild\manual-release\vc143.pdb"
) + $sources

Set-Content -LiteralPath $compileRsp -Value ($compileArgs -join "`r`n") -Encoding ASCII

$dll = Join-Path $repo "build\Release\obs-vjlink.dll"
$implib = Join-Path $repo "build\Release\obs-vjlink.lib"
$pdb = Join-Path $repo "build\Release\obs-vjlink.pdb"

$linkArgs = @(
    "/DLL",
    "/NOLOGO",
    "/MACHINE:X64",
    "/INCREMENTAL:NO",
    "/OPT:REF",
    "/OPT:ICF",
    "/OUT:""$dll""",
    "/IMPLIB:""$implib""",
    "/PDB:""$pdb""",
    """$manual\*.obj""",
    "/LIBPATH:""$obsLib""",
    "/LIBPATH:""$($msvcDir.FullName)\lib\x64""",
    "/LIBPATH:""$kitsRoot\Lib\$sdkVersion\ucrt\x64""",
    "/LIBPATH:""$kitsRoot\Lib\$sdkVersion\um\x64""",
    "obs.lib",
    "w32-pthreads.lib",
    "ws2_32.lib",
    "user32.lib",
    "shell32.lib"
)

Set-Content -LiteralPath $linkRsp -Value ($linkArgs -join "`r`n") -Encoding ASCII

& $clExe "@build\manual-release\compile.rsp"
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

& $linkExe "@build\manual-release\link.rsp"
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

function Backup-And-Deploy {
    param(
        [string]$TargetObsDir,
        [string]$Name
    )

    $destBin = Join-Path $TargetObsDir "obs-plugins\64bit"
    $destData = Join-Path $TargetObsDir "data\obs-plugins\obs-vjlink"
    New-Item -ItemType Directory -Force -Path $destBin | Out-Null
    New-Item -ItemType Directory -Force -Path $destData | Out-Null

    $backupRoot = Join-Path (Split-Path $repo -Parent) "backups"
    New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $backupZip = Join-Path $backupRoot "obs-vjlink-$Name-before-deploy-$stamp.zip"
    $stage = Join-Path $env:TEMP "obs-vjlink-$Name-backup-$stamp"
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $stage "obs-plugins\64bit") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $stage "data\obs-plugins") | Out-Null

    $oldDll = Join-Path $destBin "obs-vjlink.dll"
    $oldData = Join-Path $TargetObsDir "data\obs-plugins\obs-vjlink"
    if (Test-Path -LiteralPath $oldDll) {
        Copy-Item -LiteralPath $oldDll -Destination (Join-Path $stage "obs-plugins\64bit\obs-vjlink.dll") -Force
    }
    if (Test-Path -LiteralPath $oldData) {
        Copy-Item -LiteralPath $oldData -Destination (Join-Path $stage "data\obs-plugins") -Recurse -Force
    }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $backupZip -Force
    Remove-Item -LiteralPath $stage -Recurse -Force

    attrib -R (Join-Path $destBin "obs-vjlink.dll") 2>$null
    attrib -R (Join-Path $destData "*") /S /D 2>$null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $destBin "obs-vjlink.dll") -Force
    Copy-Item -LiteralPath (Join-Path $repo "effects") -Destination $destData -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repo "effects_meta") -Destination $destData -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repo "web-ui") -Destination $destData -Recurse -Force

    [PSCustomObject]@{
        Target = $TargetObsDir
        Backup = $backupZip
        Dll = (Get-Item -LiteralPath (Join-Path $destBin "obs-vjlink.dll")).FullName
        DllHash = (Get-FileHash -Algorithm SHA256 (Join-Path $destBin "obs-vjlink.dll")).Hash
        WebUiHash = (Get-FileHash -Algorithm SHA256 (Join-Path $destData "web-ui\vjlink-control.html")).Hash
    }
}

$results = @()
$results += Backup-And-Deploy -TargetObsDir $ObsDir -Name "obs"
if ($AlsoDeploySdk) {
    $results += Backup-And-Deploy -TargetObsDir $obsSdk -Name "sdk"
}

$results | Format-List
