Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile('D:/Temp/pb_shot.png')
$cropHeight = 90
$rect = New-Object System.Drawing.Rectangle(0, ($src.Height - $cropHeight), $src.Width, $cropHeight)
$bmp = New-Object System.Drawing.Bitmap(($src.Width * 2), ($cropHeight * 2))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$dest = New-Object System.Drawing.Rectangle(0, 0, ($src.Width * 2), ($cropHeight * 2))
$g.DrawImage($src, $dest, $rect, [System.Drawing.GraphicsUnit]::Pixel)
$bmp.Save('D:/Temp/pb_bottom.png')
$g.Dispose(); $bmp.Dispose(); $src.Dispose()
"ok $($rect.Y)"
