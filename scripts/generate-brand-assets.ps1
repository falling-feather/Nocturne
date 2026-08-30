param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$assetRoot = Join-Path $repoRoot 'assets'
$svgPath = Join-Path $assetRoot 'nocturne.svg'
$renderer = 'D:\msys64\ucrt64\bin\rsvg-convert.exe'
$sizes = @(16, 24, 32, 48, 64, 128, 256)

if (-not (Test-Path -LiteralPath $renderer)) {
    throw "找不到 SVG 渲染器：$renderer"
}
if (-not (Test-Path -LiteralPath $svgPath)) {
    throw "找不到图标源文件：$svgPath"
}

$pngs = @()
foreach ($size in $sizes) {
    $pngPath = Join-Path $assetRoot "nocturne-$size.png"
    & $renderer --width $size --height $size --output $pngPath $svgPath
    if ($LASTEXITCODE -ne 0) {
        throw "生成 ${size}px 图标失败。"
    }
    $pngs += [PSCustomObject]@{
        Size = $size
        Bytes = [System.IO.File]::ReadAllBytes($pngPath)
    }
}

$icoPath = Join-Path $assetRoot 'nocturne.ico'
$stream = [System.IO.MemoryStream]::new()
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]$pngs.Count)

    $offset = 6 + (16 * $pngs.Count)
    foreach ($png in $pngs) {
        $dimension = if ($png.Size -eq 256) { 0 } else { $png.Size }
        $writer.Write([Byte]$dimension)
        $writer.Write([Byte]$dimension)
        $writer.Write([Byte]0)
        $writer.Write([Byte]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]32)
        $writer.Write([UInt32]$png.Bytes.Length)
        $writer.Write([UInt32]$offset)
        $offset += $png.Bytes.Length
    }

    foreach ($png in $pngs) {
        $writer.Write($png.Bytes)
    }
    $writer.Flush()
    [System.IO.File]::WriteAllBytes($icoPath, $stream.ToArray())
}
finally {
    $writer.Dispose()
    $stream.Dispose()
}

Write-Host "品牌图标已生成：$icoPath"
