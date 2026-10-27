@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "CFG=all"
set "TGT=lib"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-c" goto parse_c
if /i "%~1"=="-t" goto parse_t
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--help" goto usage
echo [error] unknown argument: %~1
goto usage

:parse_c
if "%~2"=="" goto usage
set "CFG=%~2"
if /i "%CFG%"=="debug" set "CFG=Debug"
if /i "%CFG%"=="release" set "CFG=Release"
if /i "%CFG%"=="all" set "CFG=all"
shift
shift
goto parse

:parse_t
if "%~2"=="" goto usage
set "TGT=%~2"
shift
shift
goto parse

:parsed
if /i "%TGT%"=="lib" goto target_lib
if /i "%TGT%"=="sln" goto target_sln
echo [error] unknown target: %TGT%
goto usage

:target_lib
if /i "%CFG%"=="all" (
  set "CONFIGS=Debug Release"
  goto check_deps
)
if /i "%CFG%"=="debug" (
  set "CONFIGS=Debug"
  goto check_deps
)
if /i "%CFG%"=="release" (
  set "CONFIGS=Release"
  goto check_deps
)
echo [error] unknown configuration: %CFG%
goto usage

:target_sln
if /i "%CFG%"=="all" set "CFG=Debug"
if /i not "%CFG%"=="debug" (
  echo [error] -t sln supports Debug only: no gtest Release package is vendored
  goto usage
)
set "CONFIGS=Debug"
goto check_deps

:check_deps
if not exist "%ROOT%lisserver\target\x64-windows\include\asio.hpp" goto missing_deps
if not exist "%ROOT%lisserver\target\x64-windows\include\gtest\gtest.h" goto missing_deps
if not exist "%ROOT%lisserver\target\x64-windows\debug\lib\gtest.lib" goto missing_deps
if not exist "%ROOT%lisserver\target\x64-windows\debug\lib\gtest_main.lib" goto missing_deps
if /i "%TGT%"=="lib" goto find_vs2022
goto find_vs18

:missing_deps
echo [error] vendored dependency missing under lisserver\target\x64-windows
echo         refresh: conan install lisserver, then copy headers/libs per
echo         docs\2026-10-26-liblis-v143-build.md
exit /b 4

:find_vs2022
call :find_msbuild 17.0 18.0 2022
if not defined MSBUILD exit /b 3
for %%c in (%CONFIGS%) do (
  call :build_lib %%c
  if errorlevel 1 exit /b 1
)
call :cleanup_old_libs
exit /b 0

:find_vs18
call :find_msbuild 18.0 19.0 18
if not defined MSBUILD exit /b 3
echo [build] lisserver.sln Debug (v145, VS18, tests)
"%MSBUILD%" "%ROOT%lisserver\lisserver.sln" -p:Configuration=Debug -p:Platform=x64 -v:m -nologo
if errorlevel 1 exit /b 1
if not exist "%ROOT%lisserver\listest\x64\Debug\listest.exe" (
  echo [error] missing lisserver\listest\x64\Debug\listest.exe
  exit /b 1
)
echo [ok] %ROOT%lisserver\listest\x64\Debug\listest.exe
exit /b 0

:build_lib
echo [build] liblis %~1 (v143, VS2022)
"%MSBUILD%" "%ROOT%lisserver\liblis.vcxproj" -p:Configuration=%~1 -p:Platform=x64 -p:PlatformToolset=v143 -p:OutDir=lib\x64\%~1-v143\ -p:IntDir=obj\liblis\x64\%~1-v143\ -v:m -nologo
if errorlevel 1 (
  echo [error] build failed: liblis %~1
  exit /b 1
)
set "OUT=%ROOT%lisserver\lib\x64\%~1-v143"
if not exist "%OUT%\liblis.lib" (
  echo [error] missing %OUT%\liblis.lib
  exit /b 1
)
if not exist "%OUT%\liblis.pdb" (
  echo [error] missing %OUT%\liblis.pdb
  exit /b 1
)
for %%f in ("%OUT%\liblis.lib" "%OUT%\liblis.pdb") do echo [ok] %%~ff  %%~zf bytes
exit /b 0

:cleanup_old_libs
for /d %%d in ("%ROOT%lisserver\lib\x64\*") do (
  if exist "%%d\lislib.lib" del /q "%%d\lislib.lib"
  if exist "%%d\lislib.pdb" del /q "%%d\lislib.pdb"
)
if exist "%ROOT%lisserver\obj\lislib" rmdir /s /q "%ROOT%lisserver\obj\lislib"
exit /b 0

:find_msbuild
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem "call" is required: cmd /c strips the outer quotes of the backtick command
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -version "[%~1,%~2)" -requires Microsoft.Component.MSBuild -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if defined MSBUILD if not exist "%MSBUILD%" set "MSBUILD="
if defined MSBUILD exit /b 0
for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\%~3\*") do (
  if exist "%%d\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%%d\MSBuild\Current\Bin\MSBuild.exe"
)
if defined MSBUILD exit /b 0
echo [error] MSBuild for VS %~3 not found
exit /b 1

:usage
echo Usage: build.bat [-c Debug^|Release^|all] [-t lib^|sln]
echo.
echo   build.bat              Build liblis Debug+Release with VS2022 (v143)
echo   build.bat -c Debug     Build liblis Debug only (v143)
echo   build.bat -t sln       Build lisserver.sln Debug with VS18 (v145, tests)
exit /b 2
