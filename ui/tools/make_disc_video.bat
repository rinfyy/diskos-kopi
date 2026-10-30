@echo off
rem make_disc_video.bat - drag video files onto this file to make Disc-sized copies.
rem Each copy is "<name> (Disc).mp4" next to the original: 640x360 H.264, 30 fps, stereo AAC.
rem Needs FFmpeg. If it isn't installed, open PowerShell and run:  winget install Gyan.FFmpeg
setlocal
where ffmpeg >nul 2>nul
if errorlevel 1 (
  echo FFmpeg was not found.
  echo Open PowerShell and run:  winget install Gyan.FFmpeg
  echo then close this window and drag your videos onto this file again.
  pause
  exit /b 1
)
if "%~1"=="" (
  echo Drag one or more video files onto this file.
  pause
  exit /b 1
)
:next
if "%~1"=="" goto done
echo.
echo Making a Disc copy of "%~nx1" ...
ffmpeg -hide_banner -loglevel warning -stats -y -i "%~1" -map 0:v:0 -map 0:a:0? -sn ^
  -vf "scale=-2:360:flags=bicubic,fps=30,format=yuv420p" -c:v libx264 -preset veryfast -profile:v main -crf 23 ^
  -c:a aac -b:a 128k -ac 2 -movflags +faststart "%~dpn1 (Disc).mp4"
shift
goto next
:done
echo.
echo Done. Copy the "(Disc).mp4" files into the Videos folder on the SD card.
pause
