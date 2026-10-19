# (AI-assisted) Package in a fresh staging directory and publish only the validated PKG.
param(
  [string]$Toolchain = $env:OO_PS4_TOOLCHAIN,
  [string]$Eboot = (Join-Path $PSScriptRoot 'build/eboot.bin'),
  # Forja SX2: 0x00010000 = séptimo núcleo (65536 decimal); -ModoPro suma 0x00800000 (total 8454144) (modo PS4 Pro, sin probar).
  [switch]$ModoPro
)
$ErrorActionPreference = 'Stop'
if (-not $Toolchain -or -not (Test-Path -LiteralPath $Toolchain)) {
  throw 'Set OO_PS4_TOOLCHAIN to the Windows OpenOrbis Toolchain root.'
}
if (-not (Test-Path -LiteralPath $Eboot)) { throw "VM eboot not found: $Eboot" }
$pkgTool = Join-Path $Toolchain 'bin/windows/PkgTool.Core.exe'
$gp4Tool = Join-Path $Toolchain 'bin/windows/create-gp4.exe'
$sampleRoot = Join-Path $Toolchain 'samples/graphics'
$fontRoot = Join-Path $Toolchain 'samples/font/assets/fonts'
$packageRoot = Join-Path $PSScriptRoot 'build/package'
$packageDir = Join-Path $packageRoot ('staging-' + [Guid]::NewGuid().ToString('N'))
$titleId = 'FRJA00002'
# Forja SX2: the package's version is the build number (compilación 31 = 01.31, 131 = 02.31), as DolphinPS4 versions its
# packages, so a new build installs over the old one with GoldHEN's Package Installer instead of needing the app deleted.
$buildNumber = 0
if ($env:GITHUB_RUN_NUMBER) { $buildNumber = [int]$env:GITHUB_RUN_NUMBER }
$appVersion = '{0:00}.{1:00}' -f (1 + [math]::Floor($buildNumber / 100)), ($buildNumber % 100)
Write-Host "Package version: $appVersion (build $buildNumber)"
$contentId = 'IV0000-FRJA00002_00-FORJASX2EMULADOR'
$resources = @('sce_sys/about/right.sprx', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx')
$iconSource = Join-Path $PSScriptRoot 'assets/sce_sys/icon0.png'
$pic1Source = Join-Path $PSScriptRoot 'assets/sce_sys/pic1.png'
$shaderRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../bin/resources/shaders'))
$shaderFiles = @(Get-ChildItem -LiteralPath (Join-Path $shaderRoot 'vulkan'), (Join-Path $shaderRoot 'common') -File -Recurse)
if ($shaderFiles.Count -eq 0) { throw 'PCSX2 shader resources are missing.' }
$uiRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'assets/ui-png'))
$uiFiles = @(Get-ChildItem -LiteralPath $uiRoot -Filter '*.png' -File)
if ($uiFiles.Count -eq 0) { throw 'PS4SX2 UI button resources are missing.' }
$introRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'assets/intro'))
$introFiles = @(Get-ChildItem -LiteralPath $introRoot -File)
if ($introFiles.Count -lt 3) { throw 'PS4SX2 startup intro resources are missing.' }
foreach ($required in @($pkgTool, $gp4Tool, $iconSource, $pic1Source) + @($resources | ForEach-Object { Join-Path $sampleRoot $_ })) {
  if (-not (Test-Path -LiteralPath $required)) { throw "OpenOrbis dependency not found: $required" }
}
$fontSource = Join-Path $fontRoot 'Gontserrat-Regular.ttf'
if (-not (Test-Path -LiteralPath $fontSource)) { throw "OpenOrbis font not found: $fontSource" }
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir 'sce_sys/about'), (Join-Path $packageDir 'sce_module') | Out-Null
Copy-Item -LiteralPath $Eboot -Destination (Join-Path $packageDir 'eboot.bin')
foreach ($resource in $resources) {
  Copy-Item -LiteralPath (Join-Path $sampleRoot $resource) -Destination (Join-Path $packageDir $resource)
}
$iconDestination = Join-Path $packageDir 'sce_sys/icon0.png'
Copy-Item -LiteralPath $iconSource -Destination $iconDestination
$resources += 'sce_sys/icon0.png'
$pic1Destination = Join-Path $packageDir 'sce_sys/pic1.png'
Copy-Item -LiteralPath $pic1Source -Destination $pic1Destination
$resources += 'sce_sys/pic1.png'
$resources += 'resources/fonts/Gontserrat-Regular.ttf'
$packageFiles = @('eboot.bin', 'sce_sys/param.sfo') + $resources
foreach ($shaderFile in $shaderFiles) {
  $fullPath = $shaderFile.FullName
  $relative = 'resources/shaders/' + $fullPath.Substring($shaderRoot.Length + 1).Replace('\', '/')
  $destination = Join-Path $packageDir $relative
  New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName($destination)) | Out-Null
  Copy-Item -LiteralPath $shaderFile.FullName -Destination $destination
  $packageFiles += $relative
}
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir 'resources/fonts') | Out-Null
Copy-Item -LiteralPath $fontSource -Destination (Join-Path $packageDir 'resources/fonts/Gontserrat-Regular.ttf')
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir 'resources/ui') | Out-Null
foreach ($uiFile in $uiFiles) {
  $destination = Join-Path $packageDir ('resources/ui/' + $uiFile.Name)
  Copy-Item -LiteralPath $uiFile.FullName -Destination $destination
  $packageFiles += 'resources/ui/' + $uiFile.Name
}
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir 'resources/intro') | Out-Null
foreach ($introFile in $introFiles) {
  $relative = 'resources/intro/' + $introFile.Name
  Copy-Item -LiteralPath $introFile.FullName -Destination (Join-Path $packageDir $relative)
  $packageFiles += $relative
}
# (AI-assisted) Core metadata, with no user BIOS or game data in the package.
foreach ($name in @('GameIndex.yaml', 'game_controller_db.txt')) {
  $source = Join-Path $PSScriptRoot "../../bin/resources/$name"
  Copy-Item -LiteralPath $source -Destination (Join-Path $packageDir "resources/$name")
  $packageFiles += "resources/$name"
}
# Forja SX2: factory game profiles (resources/perfiles/<SERIAL>.ini), the per-game settings measured on the console.
$profileRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'assets/perfiles'))
$profileFiles = @(Get-ChildItem -LiteralPath $profileRoot -Filter '*.ini' -File)
if ($profileFiles.Count -eq 0) { throw 'Forja SX2 factory game profiles are missing.' }
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir 'resources/perfiles') | Out-Null
foreach ($profileFile in $profileFiles) {
  $relative = 'resources/perfiles/' + $profileFile.Name
  Copy-Item -LiteralPath $profileFile.FullName -Destination (Join-Path $packageDir $relative)
  $packageFiles += $relative
}
# Forja SX2 (compilation 35): factory patches (resources/parches/<SERIAL>_<CRC>_forja.pnach), which pcsx2/Patch.cpp reads after
# the player's own /data/ForjaSX2/patches. The Orbis build has no zip, so they go as loose files.
$patchRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'assets/parches'))
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir 'resources/parches') | Out-Null
foreach ($patchFile in @(Get-ChildItem -LiteralPath $patchRoot -Filter '*.pnach' -File)) {
  $relative = 'resources/parches/' + $patchFile.Name
  Copy-Item -LiteralPath $patchFile.FullName -Destination (Join-Path $packageDir $relative)
  $packageFiles += $relative
}
# Forja SX2 (compilation 55): the draw fingerprints (resources/huellas.ini), read at every game start (ps4/vm/main.cpp, LoadRules).
$rulesFile = Join-Path $PSScriptRoot 'assets/huellas.ini'
if (-not (Test-Path -LiteralPath $rulesFile)) { throw 'Forja SX2 draw fingerprints (assets/huellas.ini) are missing.' }
Copy-Item -LiteralPath $rulesFile -Destination (Join-Path $packageDir 'resources/huellas.ini')
$packageFiles += 'resources/huellas.ini'
# (AI-assisted) License notices for the bundled output resampler.
New-Item -ItemType Directory -Force -Path (Join-Path $packageDir 'resources/licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/licenses/speex-resampler.txt') -Destination (Join-Path $packageDir 'resources/licenses/speex-resampler.txt')
$packageFiles += 'resources/licenses/speex-resampler.txt'
foreach ($license in @('libchdr.txt', 'lzma.txt')) {
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot "assets/licenses/$license") -Destination (Join-Path $packageDir "resources/licenses/$license")
  $packageFiles += "resources/licenses/$license"
}
Push-Location $packageDir
try {
  & $pkgTool sfo_new sce_sys/param.sfo
  if ($LASTEXITCODE -ne 0) { throw "Could not create SFO ($LASTEXITCODE)." }
  $entries = @(
    # (AI-assisted) Restore game SFO metadata with PAID 0x3800000000000011.
    @('APP_TYPE', 'Integer', '4', '1'), @('APP_VER', 'Utf8', '8', $appVersion),
    @('ATTRIBUTE', 'Integer', '4', $(if ($ModoPro) { '8454144' } else { '65536' })), @('CATEGORY', 'Utf8', '4', 'gd'),

    @('CONTENT_ID', 'Utf8', '48', $contentId), @('DOWNLOAD_DATA_SIZE', 'Integer', '4', '0'),
    @('SYSTEM_VER', 'Integer', '4', '0'), @('TITLE', 'Utf8', '128', 'Forja SX2'),
    @('TITLE_ID', 'Utf8', '12', $titleId), @('VERSION', 'Utf8', '8', $appVersion)
  )
  foreach ($entry in $entries) {
    & $pkgTool sfo_setentry sce_sys/param.sfo $entry[0] --type $entry[1] --maxsize $entry[2] --value $entry[3] | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not set SFO field $($entry[0]) ($LASTEXITCODE)." }
  }
  & $gp4Tool -out ps4-vm-boot.gp4 "--content-id=$contentId" --files ($packageFiles -join ' ')
  if ($LASTEXITCODE -ne 0) { throw "GP4 creation failed ($LASTEXITCODE)." }
  # (AI-assisted) This create-gp4 version emits a fixed directory tree; add all packaged shader parents.
  [xml]$gp4Xml = Get-Content -LiteralPath 'ps4-vm-boot.gp4' -Raw
  foreach ($packageFile in $packageFiles) {
    $segments = $packageFile.Split('/')
    $directoryNode = $gp4Xml.psproject.rootdir
    for ($segmentIndex = 0; $segmentIndex -lt $segments.Length - 1; $segmentIndex++) {
      $segment = $segments[$segmentIndex]
      $child = $directoryNode.SelectSingleNode("dir[@targ_name='$segment']")
      if ($null -eq $child) {
        $child = $gp4Xml.CreateElement('dir')
        $child.SetAttribute('targ_name', $segment)
        $directoryNode.AppendChild($child) | Out-Null
      }
      $directoryNode = $child
    }
  }
  $gp4Xml.Save((Join-Path $packageDir 'ps4-vm-boot.gp4'))
  & $pkgTool pkg_build ps4-vm-boot.gp4 .
  if ($LASTEXITCODE -ne 0) { throw "PKG build failed ($LASTEXITCODE)." }
  $stagedPkg = Join-Path $packageDir ($contentId + '.pkg')
  & $pkgTool pkg_validate $stagedPkg
  if ($LASTEXITCODE -ne 0) { throw "PKG validation failed ($LASTEXITCODE)." }
} finally {
  Pop-Location
}
$finalPkg = Join-Path $packageRoot ($contentId + '.pkg')
Copy-Item -LiteralPath $stagedPkg -Destination $finalPkg -Force
# (AI-assisted) Keep the validated package; discard only this build's temporary tree.
$packageRootFull = [IO.Path]::GetFullPath($packageRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$stagingFull = [IO.Path]::GetFullPath($packageDir)
if (-not $stagingFull.StartsWith($packageRootFull, [StringComparison]::OrdinalIgnoreCase) -or
    (Split-Path -Leaf $stagingFull) -notmatch '^staging-[0-9a-f]{32}$') {
  throw 'Refusing to remove a staging directory outside the package output folder.'
}
Remove-Item -LiteralPath $stagingFull -Recurse -Force
Write-Output "Built and validated VM boot PKG: $finalPkg"
