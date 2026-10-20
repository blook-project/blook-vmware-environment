@echo off
rem ---------------------------------------------------------------------------
rem blook-vmware-environment: wrapper that runs the real provisioning script
rem from a SYSTEM scheduled task with full logging.
rem ---------------------------------------------------------------------------
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\blook\provision.ps1 -LogFile C:\blook\provision.log
exit /b %ERRORLEVEL%
