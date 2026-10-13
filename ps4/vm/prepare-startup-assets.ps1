# (AI-assisted) Convert the editable startup artwork into compact PS4 runtime resources.
param(
  [string]$SourceDirectory = 'C:\Users\Phoenix\Pictures\Emu',
  [string]$Python = 'py -3.12'
)
$ErrorActionPreference = 'Stop'
$assetRoot = Join-Path $PSScriptRoot 'assets'
$introRoot = Join-Path $assetRoot 'intro'
$uiRoot = Join-Path $assetRoot 'ui-png'
New-Item -ItemType Directory -Force -Path $introRoot, $uiRoot | Out-Null

$ffmpeg = $null
$native = Get-Command ffmpeg -ErrorAction SilentlyContinue
if ($native) {
  $ffmpeg = $native.Source
} else {
  $pythonParts = $Python.Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)
  $pythonExe = $pythonParts[0]
  $pythonArgs = @($pythonParts | Select-Object -Skip 1) + @('-c', 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')
  $ffmpeg = (& $pythonExe @pythonArgs).Trim()
}
if (-not $ffmpeg -or -not (Test-Path -LiteralPath $ffmpeg)) {
  throw 'FFmpeg is unavailable. Install imageio-ffmpeg with: py -3.12 -m pip install --user imageio-ffmpeg'
}

# Remove only generated frame files so replacing a shorter intro cannot retain old frames.
Get-ChildItem -LiteralPath $introRoot -Filter 'frame_*.jpg' -File -ErrorAction SilentlyContinue |
  Remove-Item -Force
$intro = Join-Path $SourceDirectory 'Intro.mp4'
if (-not (Test-Path -LiteralPath $intro)) { throw "Intro video not found: $intro" }
& $ffmpeg -y -hide_banner -loglevel error -i $intro -an `
  -vf 'scale=1280:720:flags=lanczos,fps=30' -q:v 4 (Join-Path $introRoot 'frame_%03d.jpg')
if ($LASTEXITCODE -ne 0) { throw "Intro frame conversion failed: $LASTEXITCODE" }
& $ffmpeg -y -hide_banner -loglevel error -i $intro -vn -ac 2 -ar 48000 `
  -f s16le (Join-Path $introRoot 'audio_s16le_48k_stereo.pcm')
if ($LASTEXITCODE -ne 0) { throw "Intro audio conversion failed: $LASTEXITCODE" }

$frames = @(Get-ChildItem -LiteralPath $introRoot -Filter 'frame_*.jpg' -File | Sort-Object Name)
$pcm = Get-Item -LiteralPath (Join-Path $introRoot 'audio_s16le_48k_stereo.pcm')
@(
  "frames=$($frames.Count)"
  'fps=30'
  'width=1280'
  'height=720'
  'audio_rate=48000'
  'audio_channels=2'
  "audio_bytes=$($pcm.Length)"
) | Set-Content -LiteralPath (Join-Path $introRoot 'manifest.txt') -Encoding ascii

Add-Type -AssemblyName System.Drawing
$artwork = @(
  @('PS2.png', 'loading_carousel_1.png')
  @('PS2_2.png', 'loading_carousel_2.png')
  @('ControlePs2.png', 'loading_carousel_3.png')
  @('PS4.png', 'loading_carousel_4.png')
)
foreach ($entry in $artwork) {
  $source = Join-Path $SourceDirectory $entry[0]
  $target = Join-Path $uiRoot $entry[1]
  if (-not (Test-Path -LiteralPath $source)) { throw "Carousel image not found: $source" }
  $input = [System.Drawing.Image]::FromFile($source)
  try {
    $output = [System.Drawing.Bitmap]::new(512, 512, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
      $output.SetResolution(96, 96)
      $graphics = [System.Drawing.Graphics]::FromImage($output)
      try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage($input, 0, 0, 512, 512)
      } finally { $graphics.Dispose() }
      $output.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $output.Dispose() }
  } finally { $input.Dispose() }
}

Write-Output "Prepared $($frames.Count) intro frames, $($pcm.Length) audio bytes, and $($artwork.Count) carousel images."
