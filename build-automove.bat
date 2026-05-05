@echo off

call build.bat

set gmodpath="S:\Steam\steamapps\common\GarrysMod"
pushd output
move /Y "gmcl_vrmod_win64.dll" "%gmodpath%\garrysmod\lua\bin\gmcl_vrmod_win64.dll"
move /Y "gmcl_vrmod_win32.dll" "%gmodpath%\garrysmod\lua\bin\gmcl_vrmod_win32.dll"
popd
