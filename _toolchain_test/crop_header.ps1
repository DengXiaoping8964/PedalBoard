Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile('D:/Temp/pb_shot.png')
$rect = New-Object System.Drawing.Rectangle(1030, 160, 240, 32)
$bmp = New-Object System.Drawing.Bitmap(960, 128)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$dest = New-Object System.Drawing.Rectangle(0, 0, 960, 128)
$g.DrawImage($src, $dest, $rect, [System.Drawing.GraphicsUnit]::Pixel)
$bmp.Save('D:/Temp/pb_header.png')
$g.Dispose(); $bmp.Dispose(); $src.Dispose()
"ok"
