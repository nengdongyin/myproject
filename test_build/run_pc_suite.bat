@echo off
rem Build + run the full PC test suite (needs msys64 ucrt64 on PATH for cc1.exe DLLs)
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
C:\msys64\ucrt64\bin\gcc.exe @test_build\build.rsp
if errorlevel 1 exit /b 1
test_build\pc_test_runner.exe
exit /b %ERRORLEVEL%
