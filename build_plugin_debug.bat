@echo off
setlocal
cd /d "%~dp0"

echo ============================================
echo  Step 1: Build native DLL (plugin_core.dll) - DEBUG
echo ============================================

if not exist build_plugin_debug mkdir build_plugin_debug

cmake -S . -B build_plugin_debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER=lld-link -DENABLE_CONSOLE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if errorlevel 1 goto :fail

ninja -C build_plugin_debug plugin_core
if errorlevel 1 goto :fail

cd build_plugin_debug

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

:: Also copy the PDB for symbol resolution
if exist plugin_core.pdb (
    copy /Y plugin_core.pdb "%~dp0vs_plugin\extension\GotoSlop\plugin_core.pdb"
)

del /f "%DEST%.old" >nul 2>&1

echo.
echo ============================================
echo  Step 2: Build VS extension (VSIX)
echo ============================================

cd /d "%~dp0vs_plugin\extension\GotoSlop"

dotnet build -c Debug
if errorlevel 1 goto :fail

echo.
echo ============================================
echo  DEBUG build complete!
echo ============================================

:: Find the .vsix
for /r "bin\Debug" %%f in (*.vsix) do (
    echo VSIX: %%f
    echo Double-click the .vsix to install into Visual Studio.
)

echo.
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
