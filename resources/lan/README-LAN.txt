Generals Online LAN build (experimental)
=======================================

This gives the Network (LAN) menu the same networking the Generals Online
Online menu has. No internet or server is needed.

Every player must use exactly the same build number.

Install
-------
1. Install Generals Online with its launcher once (it sets up the game files).
2. Extract this zip into your Generals Online game folder
   (the folder that has GeneralsOnlineZH.exe in it).
3. Run Allow-Through-Firewall.bat once and accept the administrator prompt.
4. Start GeneralsOnlineZH_LAN.exe and use the Network menu.
   Keep using the normal Generals Online launcher for online play.

Playing
-------
- Everyone, the host included, plays with GeneralsOnlineZH_LAN.exe from the same build.
- In the game room, the chat says "Connected to <name>" for each player.
- The host can only start once everyone is connected to everyone.
- "Cannot reach <name>": that player runs another exe, or a firewall blocks it.
  Make sure they run Allow-Through-Firewall.bat and use this build.
- "<name> is running a different game version": their files differ. Use the same zip.

Problems
--------
Send the log from every player:
  Documents\Command and Conquer Generals Zero Hour Data\GeneralsOnlineData\GeneralsOnline.log

More info: https://github.com/waheedullahkhan001/GameClient-LAN/blob/main/LAN.md
