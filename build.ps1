param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$Package,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$qtRoot = 'D:\msys64\ucrt64'
$cmake = Join-Path $qtRoot 'bin\cmake.exe'
$ninja = Join-Path $qtRoot 'bin\ninja.exe'

if (-not (Test-Path -LiteralPath $cmake)) {
    throw "找不到 Qt/MSYS2 CMake：$cmake"
}
if (-not (Test-Path -LiteralPath $ninja)) {
    throw "找不到 Ninja：$ninja"
}

# MSYS2 Qt 的 moc 在部分 Windows 区域设置下无法向中文路径写入中间文件，
# 因而只把临时构建目录放到纯英文的 LocalAppData；源码与最终包仍留在项目目录。
$buildBase = Join-Path $env:LOCALAPPDATA 'FeatherNotePrototypeBuild'
$buildDir = Join-Path $buildBase $Configuration
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$env:Path = "$(Join-Path $qtRoot 'bin');$env:Path"
& $cmake `
    -S $repoRoot `
    -B $buildDir `
    -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_PREFIX_PATH=$($qtRoot.Replace('\', '/'))"
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }

& $cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'CMake 构建失败。' }

Write-Host "构建完成：$(Join-Path $buildDir 'FeatherNote.exe')"

if (-not $SkipTests) {
    & (Join-Path $qtRoot 'bin\ctest.exe') `
        --test-dir $buildDir `
        --output-on-failure `
        -C $Configuration
    if ($LASTEXITCODE -ne 0) { throw '自动化测试失败。' }
}

if ($Package) {
    & (Join-Path $repoRoot 'scripts\package.ps1') `
        -BuildDirectory $buildDir `
        -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw '打包失败。' }
}
