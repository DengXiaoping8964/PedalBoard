param([int]$X = 0, [int]$Y = 0, [int]$W = 200, [int]$H = 100, [int]$Scale = 3,
      [string]$In = "D:\Temp\pb_shot.png", [string]$Out = "D:\Temp\pb_crop.png")
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile($In)
$bmp = New-Object System.Drawing.Bitmap($W, $H)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $W, $H)),
                 (New-Object System.Drawing.Rectangle($X, $Y, $W, $H)),
                 [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose(); $src.Dispose()
$big = New-Object System.Drawing.Bitmap($W * $Scale, $H * $Scale)
$g2 = [System.Drawing.Graphics]::FromImage($big)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g2.DrawImage($bmp, 0, 0, $W * $Scale, $H * $Scale)
$g2.Dispose(); $bmp.Dispose()
$big.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$big.Dispose()
Write-Output "saved=$Out"
