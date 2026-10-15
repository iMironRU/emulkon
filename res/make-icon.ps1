# make-icon.ps1 — значок Эмулкона (res\emulkon.ico): оранжевый квадрат со скруглением и «Э»,
# как в трее и веб-интерфейсе. ICO с PNG-кадрами 16…256 px (Windows Vista+).
#
#   .\make-icon.ps1
#
# Сохранять в UTF-8 with BOM.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$frames = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [Math]::Max(2, [int]($s * 0.22))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = $s - 1
    $path.AddArc(0, 0, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($w - 2 * $r, 0, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($w - 2 * $r, $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc(0, $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xC2, 0x41, 0x0C))
    $g.FillPath($brush, $path)

    $font = New-Object System.Drawing.Font "Segoe UI", ([single]($s * 0.62)), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $rect = New-Object System.Drawing.RectangleF 0, ([single]($s * 0.02)), $s, $s
    $g.DrawString([string][char]0x042D, $font, [System.Drawing.Brushes]::White, $rect, $fmt)

    $ms = New-Object System.IO.MemoryStream
    if ($s -ge 256) {
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)  # 256 px — PNG
    } else {
        # Меньшие — классический DIB: BITMAPINFOHEADER, BGRA снизу вверх, маска AND.
        $w2 = New-Object System.IO.BinaryWriter $ms
        $maskRow = [int]([Math]::Ceiling($s / 32.0) * 4)
        $w2.Write([uint32]40); $w2.Write([int32]$s); $w2.Write([int32](2 * $s))
        $w2.Write([uint16]1); $w2.Write([uint16]32); $w2.Write([uint32]0)
        $w2.Write([uint32]($s * $s * 4 + $maskRow * $s)); $w2.Write([int32]0); $w2.Write([int32]0)
        $w2.Write([uint32]0); $w2.Write([uint32]0)
        for ($y = $s - 1; $y -ge 0; $y--) {
            for ($x = 0; $x -lt $s; $x++) {
                $c = $bmp.GetPixel($x, $y)
                $w2.Write([byte]$c.B); $w2.Write([byte]$c.G); $w2.Write([byte]$c.R); $w2.Write([byte]$c.A)
            }
        }
        $w2.Write((New-Object byte[] ($maskRow * $s)))
        $w2.Flush()
    }
    $g.Dispose(); $bmp.Dispose()
    , @($s, $ms.ToArray())
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $s = $f[0]; $data = $f[1]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$data.Length); $bw.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($f in $frames) { $bw.Write([byte[]]$f[1]) }
$bw.Flush()
$target = Join-Path $PSScriptRoot "emulkon.ico"
[IO.File]::WriteAllBytes($target, $out.ToArray())
Write-Host "Значок: $target ($($out.Length) байт)"
