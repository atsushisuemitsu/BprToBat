@echo off
REM Build BprToBat.exe using VS2022 Build Tools (MSVC)
setlocal

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo [ERROR] vcvarsall.bat not found at: %VCVARS%
    echo Please install VS2022 Build Tools.
    exit /b 1
)

call "%VCVARS%" x86 >nul 2>&1

echo Compiling resources ...
rc /nologo BprToBat.rc
if errorlevel 1 (
    echo [ERROR] Resource compile failed.
    exit /b 1
)

echo Compiling BprToBat.exe ...
cl /EHsc /O2 /Fe:BprToBat.exe BprToBat.cpp BprToBat.res
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo Build succeeded: BprToBat.exe
