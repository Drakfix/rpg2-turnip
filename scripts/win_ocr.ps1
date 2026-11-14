# OCR a PNG with the Windows built-in OCR engine (Windows 10/11, no install needed).
# Prints one line per recognised text line: "x y w h<TAB>text" (pixel bounds).
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\win_ocr.ps1 shot.png
param([Parameter(Mandatory = $true)][string]$Path)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]

$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
function Await($op, [Type]$t) {
    $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    $task.Result
}

$full = (Resolve-Path $Path).Path
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($full)) ([Windows.Storage.StorageFile])
$stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
$bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
$result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
foreach ($line in $result.Lines) {
    $xs = $line.Words | ForEach-Object { $_.BoundingRect.X }
    $ys = $line.Words | ForEach-Object { $_.BoundingRect.Y }
    $x2 = $line.Words | ForEach-Object { $_.BoundingRect.X + $_.BoundingRect.Width }
    $y2 = $line.Words | ForEach-Object { $_.BoundingRect.Y + $_.BoundingRect.Height }
    $x = [int]($xs | Measure-Object -Minimum).Minimum
    $y = [int]($ys | Measure-Object -Minimum).Minimum
    $w = [int](($x2 | Measure-Object -Maximum).Maximum - $x)
    $h = [int](($y2 | Measure-Object -Maximum).Maximum - $y)
    "{0} {1} {2} {3}`t{4}" -f $x, $y, $w, $h, $line.Text
}
