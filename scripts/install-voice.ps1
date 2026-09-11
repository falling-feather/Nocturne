param([Parameter(Mandatory=$true)][string]$VoiceDirectory)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$voiceRoot = [IO.Path]::GetFullPath($VoiceDirectory).TrimEnd('\','/')
if ($voiceRoot -eq [IO.Path]::GetPathRoot($voiceRoot).TrimEnd('\','/')) { throw 'The voice directory must not be a drive root.' }
function VoicePath([string]$relative) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $voiceRoot $relative))
    if (-not $resolved.StartsWith($voiceRoot + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Path outside voice directory.' }
    return $resolved
}
foreach ($name in @('downloads','runtime','model','licenses')) { New-Item -ItemType Directory -Force -Path (VoicePath $name) | Out-Null }
$components = @(
    @{ Name='engine'; Url='https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/sherpa-onnx-v1.13.8-win-x64-shared-MD-MinSizeRel-no-tts.tar.bz2'; Hash='8D1A53966701B24D7353B234F0B02A07992C3433612AB42FCBAA50E4AC7EDCE4'; Bytes=16168344; Root='sherpa-onnx-v1.13.8-win-x64-shared-MD-MinSizeRel-no-tts' },
    @{ Name='model'; Url='https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-small-ctc-zh-int8-2025-04-01.tar.bz2'; Hash='B3B309F7CE4A737195FCC6963EA19B0653A7D3401580AF5AE0D3E284CBB71F0B'; Bytes=21264113; Root='sherpa-onnx-streaming-zipformer-small-ctc-zh-int8-2025-04-01' }
)
foreach ($component in $components) {
    $archive = VoicePath ('downloads/' + $component.Name + '.tar.bz2')
    if ((Test-Path -LiteralPath $archive) -and (Get-Item -LiteralPath $archive).Length -ge $component.Bytes -and (Get-FileHash -LiteralPath $archive).Hash -ne $component.Hash) {
        $invalid = VoicePath ('downloads/' + $component.Name + '.invalid-' + [Guid]::NewGuid().ToString('N') + '.tar.bz2')
        if ((Split-Path $archive -Parent) -ne (Split-Path $invalid -Parent)) { throw 'Invalid cache path.' }
        Move-Item -LiteralPath $archive -Destination $invalid
    }
    if (-not (Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive).Hash -ne $component.Hash) {
        Write-Output ('Downloading ' + $component.Name + '...')
        & curl.exe --fail --location --continue-at - --retry 3 --retry-all-errors --retry-delay 2 --connect-timeout 20 --max-time 900 --output $archive $component.Url
        if ($LASTEXITCODE -ne 0) { throw ('Download interrupted: ' + $component.Name + '. Retry to resume.') }
    }
    if ((Get-FileHash -LiteralPath $archive).Hash -ne $component.Hash) { throw ('SHA-256 verification failed: ' + $component.Name) }
    $entries = @(& tar.exe -tf $archive)
    if ($LASTEXITCODE -ne 0) { throw ('Cannot inspect archive: ' + $component.Name) }
    if ($entries | Where-Object { $_ -match '(^[\/]|^[A-Za-z]:|(^|[\/])\.\.([\/]|$))' }) { throw 'Unsafe archive path.' }
    $unpacked = VoicePath ('downloads/unpacked-' + $component.Name)
    New-Item -ItemType Directory -Force -Path $unpacked | Out-Null
    & tar.exe -xf $archive -C $unpacked
    if ($LASTEXITCODE -ne 0) { throw ('Extraction failed: ' + $component.Name) }
    $component.Source = Join-Path $unpacked $component.Root
}
foreach ($name in @('sherpa-onnx.exe','onnxruntime.dll','onnxruntime_providers_shared.dll')) {
    Copy-Item -LiteralPath (Join-Path $components[0].Source ('bin/' + $name)) -Destination (VoicePath ('runtime/' + $name)) -Force
}
foreach ($name in @('model.int8.onnx','tokens.txt')) {
    Copy-Item -LiteralPath (Join-Path $components[1].Source $name) -Destination (VoicePath ('model/' + $name)) -Force
}
foreach ($license in @('sherpa-onnx-LICENSE','onnxruntime-LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('voice-licenses/' + $license)) -Destination (VoicePath ('licenses/' + $license)) -Force
}
$inventory = @()
foreach ($directory in @('runtime','model')) {
    foreach ($file in Get-ChildItem -LiteralPath (VoicePath $directory) -File) {
        $inventory += @{ File=($directory + '/' + $file.Name); Bytes=$file.Length; Sha256=(Get-FileHash -LiteralPath $file.FullName).Hash }
    }
}
$manifest = @{Format=1;Model='Zipformer-small-CTC-zh-INT8-2025-04-01';Engine='sherpa-onnx-v1.13.8';Files=$inventory;Sources=@($components | ForEach-Object { @{Url=$_.Url;Sha256=$_.Hash} })}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (VoicePath 'installed.json') -Encoding UTF8
Write-Output 'Voice component installed. Audio stays on this computer.'
