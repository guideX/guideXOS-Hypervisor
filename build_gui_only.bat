@echo off
echo ================================================
echo Building C# GUI Only (No C++ Errors)
echo ================================================
echo.

cd "guideXOS Hypervisor GUI"

echo Cleaning previous build...
dotnet clean

echo.
echo Building GUI application...
dotnet build

echo.
echo ================================================
echo Build Complete!
echo ================================================
echo.
echo Run the application with:
echo   dotnet run
echo.
echo Or find the executable at:
echo   bin\Debug\net9.0-windows\guideXOS Hypervisor GUI.exe
echo.
pause
