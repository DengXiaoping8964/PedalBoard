Add-Type -AssemblyName System.Drawing

foreach ($name in @('drops_t1', 'drops_t2')) {
    $src = [System.Drawing.Image]::FromFile("D:/Temp/$name.png")
    $rect = New-Object System.Drawing.Rectangle(0, 700, 700, 24)
    $bmp = New-Object System.Drawing.Bitmap(1400, 52)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $dest = New-Object System.Drawing.Rectangle(0, 0, 1400, 52)
    $g.DrawImage($src, $dest, $rect, [System.Drawing.GraphicsUnit]::Pixel)
    $bmp.Save("D:/Temp/${name}_strip.png")
    $g.Dispose(); $bmp.Dispose(); $src.Dispose()
}
"ok"
