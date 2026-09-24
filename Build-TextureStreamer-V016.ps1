# TextureStreamer V016 checked release build
$ErrorActionPreference = 'Stop'

$Project = 'TextureStreamer'
$Version = 'V016'
$Marker  = 'TEXTURESTREAMER,V0_16_20260924'
$Root    = $PSScriptRoot
$OutDir  = Join-Path $Root 'verified-v016'

function Fail([string]$msg) {
    Write-Host "FAIL: $msg" -ForegroundColor Red
    exit 1
}

# layout
$required = @(
    "$Project.sln", "$Project.vcxproj", 'dllmain.cpp', 'pch.h', 'pch.cpp',
    'include\log.cpp', 'include\HookUtils.h', 'minhook\include\MinHook.h', 'minhook\src\hook.c',
    'src\AddressSet.cpp', 'src\CodePatch.cpp', 'src\PatternScan.cpp', 'src\Settings.cpp',
    'src\StallMonitor.cpp', 'src\SwapChainVram.cpp', 'src\TextureStreamerHooks.cpp', 'src\DebugConsole.cpp', 'src\PluginExport.cpp',
    "lua\${Project}_Core.lua", "lua\$Project.lua"
)
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $Root $f))) { Fail "missing $f" }
}
if (Test-Path $OutDir) { Fail "$OutDir already exists, earlier revision output is never overwritten" }

# source marker
if (-not (Select-String -Path (Join-Path $Root 'dllmain.cpp') -SimpleMatch $Marker -Quiet)) {
    Fail "marker $Marker not in dllmain.cpp"
}

# MSBuild
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found' }
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild -or -not (Test-Path $msbuild)) { Fail 'MSBuild.exe not found' }
Write-Host "MSBuild: $msbuild"

# clean Release x64 rebuild
& $msbuild (Join-Path $Root "$Project.sln") /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { Fail "MSBuild exit code $LASTEXITCODE" }

$dll = Join-Path $Root "x64\Release\$Project.dll"
if (-not (Test-Path $dll)) { Fail "$dll not built" }

# marker in binary
$text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dll))
if (-not $text.Contains($Marker)) { Fail "marker $Marker not in $dll" }

# stage IH release layout
$modules = Join-Path $OutDir 'mod\modules'
$plugins = Join-Path $OutDir 'plugins'
New-Item -ItemType Directory -Path $modules, $plugins -Force | Out-Null
Copy-Item (Join-Path $Root "lua\${Project}_Core.lua") $modules
Copy-Item (Join-Path $Root "lua\$Project.lua") $plugins
Copy-Item $dll $plugins

$staged = @("mod\modules\${Project}_Core.lua", "plugins\$Project.dll", "plugins\$Project.lua")
foreach ($f in $staged) {
    if (-not (Test-Path (Join-Path $OutDir $f))) { Fail "staging missing $f" }
}

$hash = (Get-FileHash (Join-Path $plugins "$Project.dll") -Algorithm SHA256).Hash
Write-Host ''
Write-Host "PASS: $Project $Version" -ForegroundColor Green
Write-Host "  $Project.dll SHA256 $hash"
Write-Host "  Install: close the game, copy the contents of $OutDir into the game root"
Write-Host "  Logs:    plugins\${Project}_loader.log (Core module), $Project.log beside mgsvtpp.exe (${Project}_boot.log and usage lines only with debugLog = true)"
Write-Host "  Loaded:  $Project.log starts with '[DLL] InitThread started. $Marker'"
Write-Host '  Hotkeys: none (debugWindow in plugins\TextureStreamer.lua opens the live console)'
exit 0
