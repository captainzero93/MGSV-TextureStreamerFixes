@echo off
rem TextureStreamer V018 checked build launcher
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-TextureStreamer-V018.ps1"
set RC=%ERRORLEVEL%
pause
exit /b %RC%
