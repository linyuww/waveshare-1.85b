$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$fontPath = Join-Path $projectRoot '.cache/fonts/NotoSansSC-Regular.otf'
if (!(Test-Path -LiteralPath $fontPath)) {
    New-Item -ItemType Directory -Path (Split-Path $fontPath) -Force | Out-Null
    Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf' -OutFile $fontPath
}
$outputPath = Join-Path $projectRoot 'firmware/main/ui_font.c'
# Include common CJK glyphs so Chinese SSIDs can be displayed as well as the interface.
npx.cmd --yes lv_font_conv@1.5.3 --font $fontPath --range '32-126,0xa0-0xff,0x2000-0x206f,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef' --size 18 --bpp 2 --no-kerning --no-compress --format lvgl --lv-font-name ui_font --lv-fallback lv_font_montserrat_18 --output $outputPath
if ($LASTEXITCODE -ne 0) { throw 'Font generation failed.' }
# Rare fullwidth glyphs have extreme extents; use a consistent line box for this small UI.
$fontSource = Get-Content -LiteralPath $outputPath -Raw
$fontSource = $fontSource -replace '\.line_height = \d+', '.line_height = 24' -replace '\.base_line = \d+', '.base_line = 5'
Set-Content -LiteralPath $outputPath -Value $fontSource -Encoding utf8
