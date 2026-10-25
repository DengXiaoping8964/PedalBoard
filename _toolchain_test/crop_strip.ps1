Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile('D:/Temp/pb_shot.png')
$cropHeight = 24
$rect = New-Object System.Drawing.Rectangle(0, ($src.Height - $cropHeight), 620, $cropHeight)
$bmp = New-Object System.Drawing.Bitmap(2480, 96)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$dest = New-Object System.Drawing.Rectangle(0, 0, 2480, 96)
$g.DrawImage($src, $dest, $rect, [System.Drawing.GraphicsUnit]::Pixel)
$bmp.Save('D:/Temp/pb_strip.png')
$g.Dispose(); $bmp.Dispose(); $src.Dispose()
"ok"
