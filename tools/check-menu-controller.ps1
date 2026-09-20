$ErrorActionPreference = 'Stop'
$ddcRoot = Split-Path $PSScriptRoot -Parent
$revision = 'd448520c07dec8430e29adad4337fcf4319847a7'
$checkRoot = Join-Path $ddcRoot 'build/menu-controller'
$sourceRoot = Join-Path $checkRoot "upstream-$revision"
$files = @('src/GamepadNavigation.cpp', 'include/GamepadNavigation.h',
    'imgui/imgui.cpp', 'imgui/imgui.h', 'imgui/imgui_internal.h', 'imgui/imconfig.h',
    'imgui/imgui_draw.cpp', 'imgui/imgui_tables.cpp', 'imgui/imgui_widgets.cpp',
    'imgui/imstb_rectpack.h', 'imgui/imstb_textedit.h', 'imgui/imstb_truetype.h',
    'public/SKSE/plugins/fonts/fa-solid-900.ttf')
foreach ($file in $files) {
    $destination = Join-Path $sourceRoot $file
    if (-not (Test-Path -LiteralPath $destination)) {
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/QTR-Modding/SKSE-Menu-Framework-3/$revision/$file" -OutFile $destination
    }
}
cmake -S (Join-Path $ddcRoot 'tests/menu-controller') -B (Join-Path $checkRoot 'build') -G 'Visual Studio 18 2026' -A x64 "-DSMF_SOURCE_DIR=$sourceRoot"
if ($LASTEXITCODE) { throw 'Menu controller check configuration failed.' }
cmake --build (Join-Path $checkRoot 'build') --config Release --parallel 4
if ($LASTEXITCODE) { throw 'Menu controller check build failed.' }
ctest --test-dir (Join-Path $checkRoot 'build') -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Menu controller compatibility checks failed.' }
