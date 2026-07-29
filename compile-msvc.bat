@echo off

cd /D "%~dp0"

if not exist build-win32 (
    echo "Created build"
    mkdir build-win32
)
cd build-win32

:: Try Visual Studio 2019 first, then fall back to 2022
cmake -G "Visual Studio 16 2019" -A x64 .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 2>nul
if errorlevel 1 (
    echo "VS 2019 generator failed, trying VS 2022..."
    cmake -G "Visual Studio 17 2022" -A x64 .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

cmake --build . --config Release
cd ..