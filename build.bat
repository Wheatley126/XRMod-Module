@echo off

REM ***************************** edit this with the correct path to vcvarsall.bat ******************************

set vcvarsallpath="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"

REM ******************************************************************************************************************

if exist deps\ goto build
echo Dependencies not found. Press any key to attempt download.
pause
mkdir deps 2>NUL
mkdir deps\gmod 2>NUL
mkdir deps\openxr 2>NUL
pushd deps\gmod
powershell -command [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest https://github.com/Facepunch/gmod-module-base/archive/15bf18f369a41ac3d4eba29ee0679f386ec628b7.zip -Out tmp.zip; Expand-Archive tmp.zip -Force; Move-Item tmp\gmod-module-base-15bf18f369a41ac3d4eba29ee0679f386ec628b7\include\GarrysMod\Lua\* -Force; Remove-Item tmp.zip; Remove-Item tmp -Recurse;
popd
pushd deps\openxr
powershell -command [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/e2da9ce83a4388c9622da328bf48548471261290/include/openxr/openxr.h -Out openxr.h;
popd
echo Download complete (if there are no errors above). Press any key to attempt build.
pause

:build

set CompilerFlags= -MT -nologo -Oi -O2 -W3 /wd4996 /std:c++20 /permissive /I ..\deps /I ..\deps\garrysmod_common\include /I ..\deps\garrysmod_common\helpers\include /I ..\deps\garrysmod_common\helpers\source /I ..\deps\garrysmod_common\helpers_extended\include /I ..\deps\garrysmod_common\scanning\include\scanning /I ..\deps\garrysmod_common\scanning\source\windows /I ../deps/garrysmod_common/detouring/include/detouring  /I ../deps/garrysmod_common/detouring/source /I ..\deps\garrysmod_common\detouring\minhook\include /I ..\deps\garrysmod_common\detouring\minhook\src /I ..\deps\garrysmod_common\detouring\minhook\src\hde /I ..\deps\garrysmod_common\sourcesdk-minimal\public /I ..\deps\garrysmod_common\sourcesdk-minimal\public\tier0 /I ..\deps\garrysmod_common\sourcesdk-minimal\public\tier1 /I ..\deps\garrysmod_common\sourcesdk-minimal\tier2
set LinkerFlags= -INCREMENTAL:NO -opt:ref d3d11.lib USER32.LIB Shell32.lib /LIBPATH:..\deps\garrysmod_common /LIBPATH:..\deps\openxr /DLL

if not exist output\ mkdir output

pushd output
call %vcvarsallpath% x64
cl %CompilerFlags% ..\src\vrmod.cpp /link %LinkerFlags% openxr_loader_win64.lib /out:gmcl_vrmod_win64.dll
call %vcvarsallpath% x86
cl %CompilerFlags% ..\src\vrmod.cpp /link %LinkerFlags% openxr_loader_win32.lib /out:gmcl_vrmod_win32.dll
del gmcl_vrmod_win64.exp
del gmcl_vrmod_win64.lib
del gmcl_vrmod_win32.exp
del gmcl_vrmod_win32.lib
del vrmod.obj
popd
