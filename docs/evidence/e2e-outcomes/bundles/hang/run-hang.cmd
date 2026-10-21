@echo off
copy /y "%~dp0guest-hang-driver.sys" C:\Windows\System32\drivers\bvmenohang.sys >nul
sc create bvmenohang type= kernel binPath= C:\Windows\System32\drivers\bvmenohang.sys >nul 2>&1
sc start bvmenohang
exit /b 0
