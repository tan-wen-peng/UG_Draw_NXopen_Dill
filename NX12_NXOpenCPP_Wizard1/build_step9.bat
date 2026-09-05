@echo off
REM ============================================================
REM Build NX12_Step9_CloudLines only (Release x64)
REM Output: bin\Release\NX12_Step9_CloudLines.dll
REM ============================================================
set MSBUILD="D:\Program Files (x86)\Microsoft Visual Studio\2017\Community\MSBuild\15.0\Bin\MSBuild.exe"
set SOLUTION=%~dp0NX12_MultiModule.sln

if not exist %MSBUILD% (
  echo MSBuild not found: %MSBUILD%
  echo Open NX12_MultiModule.sln in Visual Studio and build the
  echo NX12_Step9_CloudLines project with Release x64 instead.
  exit /b 1
)

%MSBUILD% "%SOLUTION%" /t:NX12_Step9_CloudLines /p:Configuration=Release /p:Platform=x64 /m /v:minimal

if %ERRORLEVEL% EQU 0 (
  echo.
  echo ========================================
  echo BUILD OK
  echo DLL: %~dp0bin\Release\NX12_Step9_CloudLines.dll
  echo ========================================
) else (
  echo.
  echo ========================================
  echo BUILD FAILED - see errors above
  echo ========================================
  exit /b 1
)
