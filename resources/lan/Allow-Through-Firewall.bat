@echo off
rem Lets GeneralsOnlineZH_LAN.exe through Windows Firewall on every network type.
rem Virtual LAN adapters (ZeroTier, Radmin VPN, Tailscale) are often marked "Public", which blocks LAN games.

net session >nul 2>&1
if errorlevel 1 (
    echo Asking for administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "GAME_EXE=%~dp0GeneralsOnlineZH_LAN.exe"
if not exist "%GAME_EXE%" (
    echo Could not find "%GAME_EXE%".
    echo Put this file in the same folder as GeneralsOnlineZH_LAN.exe and run it again.
    pause
    exit /b 1
)

netsh advfirewall firewall delete rule name="Generals Online LAN" >nul 2>&1
netsh advfirewall firewall add rule name="Generals Online LAN" dir=in action=allow program="%GAME_EXE%" enable=yes profile=any >nul
netsh advfirewall firewall add rule name="Generals Online LAN" dir=out action=allow program="%GAME_EXE%" enable=yes profile=any >nul

echo Done. Windows Firewall now allows "%GAME_EXE%" on all networks.
pause
