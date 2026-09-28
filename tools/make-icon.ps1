# Generates src\DriveScanner.ico (rings motif) at 16/24/32/48/256 px, PNG-encoded entries.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\src\DriveScanner.ico'
$sizes = 16, 24, 32, 48, 256
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $c = $s / 2.0
    $rings = @(
        @{ r = $s * 0.47; w = $s * 0.16 },
        @{ r = $s * 0.31; w = $s * 0.14 }
    )
    $hues = @(215, 150, 35, 330, 265, 190)
    $spans = @(110, 70, 55, 45, 40, 40)
    foreach ($ri in 0..1) {
        $ring = $rings[$ri]
        $r = $ring.r - $ring.w / 2
        $a = -90.0
        for ($i = 0; $i -lt $spans.Count; $i++) {
            $sweep = $spans[$i] * (1 - 0.25 * $ri)
            if ($ri -eq 1 -and $i -gt 3) { break }
            $h = $hues[$i] / 360.0; $sat = 0.62; $l = 0.52 + 0.1 * $ri
            $q = if ($l -lt 0.5) { $l * (1 + $sat) } else { $l + $sat - $l * $sat }; $p = 2 * $l - $q
            $rgb = foreach ($t in ($h + 1/3), $h, ($h - 1/3)) {
                if ($t -lt 0) { $t += 1 }; if ($t -gt 1) { $t -= 1 }
                $v = if ($t -lt 1/6) { $p + ($q - $p) * 6 * $t } elseif ($t -lt 1/2) { $q } elseif ($t -lt 2/3) { $p + ($q - $p) * (2/3 - $t) * 6 } else { $p }
                [int]([math]::Round($v * 255))
            }
            $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, $rgb[0], $rgb[1], $rgb[2])), $ring.w
            $gap = if ($s -le 24) { 0 } else { 2 }
            $g.DrawArc($pen, [single]($c - $r), [single]($c - $r), [single](2 * $r), [single](2 * $r), [single]$a, [single]($sweep - $gap))
            $pen.Dispose()
            $a += $sweep
        }
    }
    $core = $s * 0.14
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 70, 76, 88))), [single]($c - $core), [single]($c - $core), [single](2 * $core), [single](2 * $core))
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Close()
"Wrote $out"
