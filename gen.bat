:: Generate a Visual Studio solution for crivate using the CMake that ships with VS.
::
:: Usage:
::   gen.bat [--clean] [cmake args...]
::
::   --clean    Remove the existing build directory before generating.
::   Extra args are forwarded to CMake (for example -DCMAKE_VERBOSE_MAKEFILE=ON).
::
:: Examples:
::   gen.bat
::   gen.bat --clean
::
:: Picks the latest installed Visual Studio that has the MSVC x64 toolset,
:: then uses that install's CMake. Always generates an x64 solution.

@echo off
setlocal enableextensions enabledelayedexpansion

cd /d "%~dp0"

set CLEAN_BUILD=0
set CMAKE_ARGS=

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--clean" (
	set CLEAN_BUILD=1
	shift
	goto parse_args
)
set CMAKE_ARGS=!CMAKE_ARGS! %1
shift
goto parse_args
:args_done

if %CLEAN_BUILD%==1 (
	if exist "build" (
		echo Removing existing build directory...
		rmdir /s /q "build"
	)
)

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
	echo ERROR: vswhere.exe not found at "%VSWHERE%"
	exit /b 1
)

for /f "usebackq tokens=*" %%i in (`
	"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
`) do (
	set VS_DIR=%%i
)

if not defined VS_DIR (
	echo ERROR: Visual Studio is not installed or does not have C++ tools.
	exit /b 1
)

echo Found Visual Studio at: %VS_DIR%

set VS_CMAKE=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
if not exist "%VS_CMAKE%" (
	echo ERROR: CMake not found in Visual Studio, expected it here: "%VS_CMAKE%"
	exit /b 1
)

echo Using Visual Studio CMake at: %VS_CMAKE%

call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
	echo ERROR: Failed to run vcvars64.bat
	exit /b 1
)

:: CMake generator names are "Visual Studio <major> <year>", not the product-line
:: value vswhere reports (that is "18" for VS 2026, not "2026").
for /f "usebackq tokens=*" %%v in (`
	"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
`) do (
	set VS_VER=%%v
)

set VS_MAJOR=
for /f "tokens=1 delims=." %%a in ("!VS_VER!") do set VS_MAJOR=%%a

set GENERATOR=
if "!VS_MAJOR!"=="16" set GENERATOR=Visual Studio 16 2019
if "!VS_MAJOR!"=="17" set GENERATOR=Visual Studio 17 2022
if "!VS_MAJOR!"=="18" set GENERATOR=Visual Studio 18 2026

set CMAKE_GEN_ARGS=-A x64
if defined GENERATOR (
	set CMAKE_GEN_ARGS=-G "!GENERATOR!" -A x64
	echo Using CMake generator: !GENERATOR! x64
) else (
	echo WARNING: Unknown Visual Studio version !VS_VER!, letting CMake pick the generator.
	echo Using CMake architecture: x64
)

if defined CMAKE_ARGS (
	echo Using extra CMake args:!CMAKE_ARGS!
)

echo.
echo --------------------------------------------------------------------------------
echo Running CMake command:
echo "%VS_CMAKE%" !CMAKE_GEN_ARGS! -S . -B build!CMAKE_ARGS!
echo --------------------------------------------------------------------------------
echo.
"%VS_CMAKE%" !CMAKE_GEN_ARGS! -S . -B build!CMAKE_ARGS!
if errorlevel 1 (
	echo ERROR: CMake generation failed.
	exit /b 1
)

:: Optional compile_commands.json for clangd / Cursor. Failure does not fail gen.bat.
set VS_NINJA=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
if exist "%VS_NINJA%" (
	echo.
	echo --------------------------------------------------------------------------------
	echo Running Ninja CMake for compile_commands.json:
	echo "%VS_CMAKE%" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_MAKE_PROGRAM="%VS_NINJA%" -S . -B build/build-ninja!CMAKE_ARGS!
	echo --------------------------------------------------------------------------------
	echo.
	"%VS_CMAKE%" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_MAKE_PROGRAM="%VS_NINJA%" -S . -B build/build-ninja!CMAKE_ARGS!
	if errorlevel 1 (
		echo WARNING: Ninja configuration failed. compile_commands.json will not be available.
	) else (
		"%VS_CMAKE%" -E copy_if_different build/build-ninja/compile_commands.json compile_commands.json
		if errorlevel 1 (
			echo WARNING: Could not copy compile_commands.json to the project root.
		) else (
			echo compile_commands.json is in the project root ^(and in build\build-ninja\^).
		)
	)
) else (
	echo WARNING: Ninja not found in Visual Studio, skipping compile_commands.json.
)

echo.
echo Done. Visual Studio solution is in build\
echo Build with: cmake --build build --config Release
echo Or open the solution in Visual Studio and build the Release configuration.
exit /b 0
