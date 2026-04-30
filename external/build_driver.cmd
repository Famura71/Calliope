@echo off
call "C:\My_Files\Work\IDEs\Visual_Studio\VC\Auxiliary\Build\vcvars64.bat"
"C:\My_Files\Work\IDEs\Visual_Studio\MSBuild\Current\Bin\amd64\MSBuild.exe" "c:\My_Files\Work\Projects\Shared\Calliope\external\Virtual-Display-Driver\Virtual-Audio-Driver (Latest Stable)\VirtualAudioDriver.sln" /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
