@echo off

call build-automove.bat
set gmodpath="S:\Steam\steamapps\common\GarrysMod"
"%gmodpath%\..\..\..\steam.exe" -applaunch 4000 -dev -noworkshop +maxplayers "1" +map "gm_construct"
