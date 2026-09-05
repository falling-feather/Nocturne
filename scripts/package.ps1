param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidatePattern('^$|^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$DirectoryName = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$qtRoot = 'D:\msys64\ucrt64'
$qtBin = Join-Path $qtRoot 'bin'
$qtPlugins = Join-Path $qtRoot 'share\qt6\plugins'
$ldd = 'D:\msys64\usr\bin\ldd.exe'
$distRoot = Join-Path $repoRoot 'dist'
$cmakeContents = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeContents -notmatch 'project\s*\(Nocturne\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw '无法从 CMakeLists.txt 解析夜航版本号。'
}
$appVersion = $Matches[1]
$packageName = if ($DirectoryName) {
    $DirectoryName
} elseif ($Configuration -eq 'Release') {
    "Nocturne-$appVersion-portable"
} else {
    "Nocturne-$appVersion-debug"
}
$packageDir = Join-Path $distRoot $packageName

$resolvedDist = [System.IO.Path]::GetFullPath($distRoot).TrimEnd('\') + '\'
$resolvedPackage = [System.IO.Path]::GetFullPath($packageDir)
if (-not $resolvedPackage.StartsWith($resolvedDist, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "拒绝清理非 dist 目录：$resolvedPackage"
}

if (Test-Path -LiteralPath $resolvedPackage) {
    Remove-Item -LiteralPath $resolvedPackage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $resolvedPackage | Out-Null

$exe = Join-Path $BuildDirectory 'Nocturne.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw "找不到构建产物：$exe"
}
Copy-Item -LiteralPath $exe -Destination (Join-Path $resolvedPackage 'Nocturne.exe')
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\qt.conf') -Destination $resolvedPackage

$qtLibraries = @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Sql.dll')
foreach ($library in $qtLibraries) {
    Copy-Item -LiteralPath (Join-Path $qtBin $library) -Destination $resolvedPackage
}

$pluginFiles = @{
    'platforms'   = @('qwindows.dll')
    'sqldrivers'  = @('qsqlite.dll')
    'imageformats' = @('qgif.dll', 'qico.dll', 'qjpeg.dll')
}
foreach ($folder in $pluginFiles.Keys) {
    $destination = Join-Path $resolvedPackage $folder
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    foreach ($plugin in $pluginFiles[$folder]) {
        Copy-Item -LiteralPath (Join-Path (Join-Path $qtPlugins $folder) $plugin) -Destination $destination
    }
}

if (-not (Test-Path -LiteralPath $ldd)) {
    throw "找不到依赖扫描器：$ldd"
}

# windeployqt 的 MSYS2 构建不会复制 UCRT64 的非 Qt 依赖；递归扫描已选二进制，
# 只把 /ucrt64/bin 中实际用到的 DLL 放入包根目录。
$env:Path = "$resolvedPackage;$qtBin;D:\msys64\usr\bin;$env:Path"
$scanned = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
do {
    $copiedAny = $false
    $binaries = Get-ChildItem -LiteralPath $resolvedPackage -Recurse -File |
        Where-Object { $_.Extension -ieq '.dll' -or $_.Extension -ieq '.exe' }
    foreach ($binary in $binaries) {
        if (-not $scanned.Add($binary.FullName)) { continue }
        $lines = & $ldd $binary.FullName 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "依赖扫描失败：$($binary.FullName)`n$($lines -join [Environment]::NewLine)"
        }
        foreach ($line in $lines) {
            if ($line -match '=>\s+/ucrt64/bin/([^\s]+\.dll)') {
                $name = $Matches[1]
                $source = Join-Path $qtBin $name
                $destination = Join-Path $resolvedPackage $name
                if (-not (Test-Path -LiteralPath $destination)) {
                    Copy-Item -LiteralPath $source -Destination $destination
                    $copiedAny = $true
                }
            }
        }
    }
} while ($copiedAny)

# 随公开分发物带上供应商原始许可和精确版本源码包入口。
python (Join-Path $repoRoot 'scripts\collect-runtime-notices.py') --package-dir $resolvedPackage
if ($LASTEXITCODE -ne 0) { throw '运行库许可收集失败。' }
Copy-Item -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md') -Destination $resolvedPackage
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\QUICKSTART.txt') -Destination $resolvedPackage

$files = Get-ChildItem -LiteralPath $resolvedPackage -Recurse -File
$totalBytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ("便携测试包：{0}" -f $resolvedPackage)
Write-Host ("文件：{0} 个；大小：{1:N1} MiB" -f $files.Count, ($totalBytes / 1MB))
