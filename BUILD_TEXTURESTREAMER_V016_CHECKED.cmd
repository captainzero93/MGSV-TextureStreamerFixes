@echo off
rem TextureStreamer V016 checked build launcher
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-TextureStreamer-V016.ps1"
set RC=%ERRORLEVEL%
pause
exit /b %RC%
