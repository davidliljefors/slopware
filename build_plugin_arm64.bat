@echo off
setlocal
cd /d "%~dp0"

:: Read version from VERSION file
set /p GOTOSLOP_VERSION=<VERSION
if "%GOTOSLOP_VERSION%"=="" (
    echo ERROR: Could not read VERSION file
    goto :fail
)
echo Version: %GOTOSLOP_VERSION%

echo ============================================
echo  Step 1: Build native DLL (plugin_core.dll) [ARM64]
echo ============================================

if not exist build_plugin_arm64 mkdir build_plugin_arm64

cmake -S . -B build_plugin_arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER=lld-link -DCMAKE_C_COMPILER_TARGET=aarch64-pc-windows-msvc -DCMAKE_CXX_COMPILER_TARGET=aarch64-pc-windows-msvc -DTARGET_ARCH=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if errorlevel 1 goto :fail

ninja -C build_plugin_arm64 plugin_core
if errorlevel 1 goto :fail

cd build_plugin_arm64

if not exist plugin_core.dll (
    echo ERROR: plugin_core.dll not found after build
    goto :fail
)

echo Copying plugin_core.dll into C# project...
set "DEST=%~dp0vs_plugin\extension\GotoSlop\plugin_core.dll"
if exist "%DEST%" (
    del /f "%DEST%.old" >nul 2>&1
    rename "%DEST%" plugin_core.dll.old >nul 2>&1
)
copy /Y plugin_core.dll "%DEST%"
if errorlevel 1 goto :fail
del /f "%DEST%.old" >nul 2>&1

echo.
echo ============================================
echo  Step 2: Build VS extension (VSIX) v%GOTOSLOP_VERSION%
echo ============================================

cd /d "%~dp0vs_plugin\extension\GotoSlop"

dotnet build -c Release /p:Version=%GOTOSLOP_VERSION%
if errorlevel 1 goto :fail

:: Find the .vsix
set "VSIX_PATH="
for /r "bin\Release" %%f in (*.vsix) do set "VSIX_PATH=%%f"

if "%VSIX_PATH%"=="" (
    echo ERROR: No .vsix found after build
    goto :fail
)

echo.
echo ============================================
echo  Step 3: Sign VSIX
echo ============================================

if defined SIGN_CERT_PFX (
    echo Signing %VSIX_PATH% ...
    dotnet sign code azure-key-vault "%VSIX_PATH%" --timestamp-url http://timestamp.digicert.com -d "GotoSlop" -u "https://github.com/GotoSlop" --certificate "%SIGN_CERT_PFX%" --password "%SIGN_CERT_PASSWORD%"
    if errorlevel 1 (
        echo WARNING: VSIX signing failed. The VSIX is unsigned.
    ) else (
        echo VSIX signed successfully.
    )
) else (
    echo SIGN_CERT_PFX not set, skipping VSIX signing.
    echo To sign, set SIGN_CERT_PFX and SIGN_CERT_PASSWORD environment variables.
)

echo.
echo ============================================
echo  Build complete! [ARM64] v%GOTOSLOP_VERSION%
echo ============================================
echo VSIX: %VSIX_PATH%

echo.
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
