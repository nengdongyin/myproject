@echo off
rem Build only (no run). Temporary helper.
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
C:\msys64\ucrt64\bin\gcc.exe @test_build\build.rsp
exit /b %ERRORLEVEL%
