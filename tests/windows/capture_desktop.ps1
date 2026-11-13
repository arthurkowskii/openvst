# Capture the interactive desktop so a blocked dialog can be identified.
# Runs inside the user's own session via a scheduled task, never from SSH.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$path = Join-Path $env:LOCALAPPDATA 'OpenVST-test\desktop.png'
$bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()
$names = Get-Process | Where-Object { $_.MainWindowTitle } | ForEach-Object { $_.ProcessName + ' | ' + $_.MainWindowTitle }
$names | Set-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'OpenVST-test\windows.txt') -Encoding UTF8
