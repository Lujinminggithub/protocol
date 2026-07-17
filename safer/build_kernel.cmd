@echo off
set MSBUILD="D:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe"
%MSBUILD% "E:\project\safer\src\kernel\PersonalSafer.vcxproj" /t:Build /p:Configuration=Release /p:Platform=x64 /p:TrackFileAccess=false /p:SkipPackageVerification=true /v:minimal
