@echo off
setlocal

set BUILD_DIR=build_dev_release

if not exist %BUILD_DIR% mkdir %BUILD_DIR%

cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER=lld-link -DENABLE_CONSOLE=ON
if errorlevel 1 (
    echo CMake configure failed.
    exit /b 1
)

ninja -C %BUILD_DIR%
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo Build succeeded: %BUILD_DIR%\gotofile.exe
