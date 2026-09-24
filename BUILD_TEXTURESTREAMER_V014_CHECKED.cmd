@echo off
rem TextureStreamer V014 checked build launcher
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-TextureStreamer-V014.ps1"
set RC=%ERRORLEVEL%
pause
exit /b %RC%
