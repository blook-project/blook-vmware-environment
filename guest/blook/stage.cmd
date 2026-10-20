@echo off
rem ---------------------------------------------------------------------------
rem blook-vmware-environment: runs once inside the guest during the
rem "specialize" pass (as SYSTEM). It copies the provisioning payload off the
rem answer media and registers a boot-triggered SYSTEM task so provisioning
rem survives OOBE reboots.
rem ---------------------------------------------------------------------------
setlocal
set SRC=%~dp0
if not exist C:\blook mkdir C:\blook
copy /y "%SRC%provision.ps1" C:\blook\provision.ps1 >nul
copy /y "%SRC%stage.cmd" C:\blook\stage.cmd >nul
copy /y "%SRC%provision.cmd" C:\blook\provision.cmd >nul
schtasks /create /tn blook-provision /tr "C:\blook\provision.cmd" /sc onstart /ru SYSTEM /rl HIGHEST /f >nul 2>&1
schtasks /run /tn blook-provision >nul 2>&1
exit /b 0
