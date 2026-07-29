@echo off
echo Downloading Mesa3D 26.1.3 MSVC (69 MB)...
echo This may take a few minutes depending on your internet speed.
cd /D "%~dp0"
powershell -Command "& {$wc = New-Object System.Net.WebClient; $wc.DownloadFile('https://github.com/pal1000/mesa-dist-win/releases/download/26.1.3/mesa3d-26.1.3-release-msvc.7z', 'mesa3d-26.1.3-release-msvc.7z'); Write-Host 'Downloaded. Extracting...'; if (Test-Path 'C:\Program Files\7-Zip\7z.exe') { & 'C:\Program Files\7-Zip\7z' x mesa3d-26.1.3-release-msvc.7z -y '*/x64/opengl32.dll' '*/x64/mesa_driver_llvmpipe.dll' '*/x64/LLVM-C.dll' 2^>$null; Move-Item -Path 'mesa3d-26.1.3-release-msvc/x64/*.dll' -Destination '.' -Force; Remove-Item -Path 'mesa3d-26.1.3-release-msvc' -Recurse -Force; Remove-Item -Path 'mesa3d-26.1.3-release-msvc.7z' -Force; } else { Write-Host 'Extract manually with 7-Zip'; } Write-Host 'Done!';}"
