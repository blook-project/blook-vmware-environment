@echo off
copy /y "%~dp0guest-bsod-driver.sys" C:\Windows\System32\drivers\bvmenobsod.sys >nul
sc create bvmenobsod type= kernel binPath= C:\Windows\System32\drivers\bvmenobsod.sys >nul 2>&1
sc start bvmenobsod
exit /b %ERRORLEVEL%
