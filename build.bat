@echo off
setlocal
rem Compila el plugin y lo copia a la carpeta Extra Effects de Resolume Arena.
rem Requiere Visual Studio 2022 con "Desarrollo para el escritorio con C++" (incluye CMake y el Windows SDK).

cd /d "%~dp0"

for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')"`) do set "DOCS=%%D"
set "EXTRA=%DOCS%\Resolume Arena\Extra Effects"

where cmake >nul 2>nul
if errorlevel 1 (
    echo No encuentro cmake. Abre "Developer Command Prompt for VS 2022" y vuelve a ejecutar build.bat
    exit /b 1
)

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DRESOLUME_EXTRA_EFFECTS_DIR="%EXTRA%"
if errorlevel 1 exit /b 1

cmake --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo Listo: build\Release\ScreenCapture.dll
echo Copiado a: %EXTRA%
echo Cierra y vuelve a abrir Resolume Arena para que cargue el plugin.
