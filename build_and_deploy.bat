@echo off
REM ============================================================
REM  twp toolbox - one-click build & deploy (NX12, Release x64)
REM  Builds all 5 solutions, then copies the 12 DLLs into
REM  E:\UG\nx_app\application\
REM ============================================================
setlocal

set "MSBUILD=D:\Program Files (x86)\Microsoft Visual Studio\2017\Community\MSBuild\15.0\Bin\MSBuild.exe"
set "UGII_BASE_DIR=D:\Program Files\Siemens\NX 12.0"
set "DEPLOY=E:\UG\nx_app\application"

if not exist "%MSBUILD%" goto :nomsbuild

echo ============================================================
echo  twp toolbox - NX12 multi-module build (Release x64)
echo ============================================================

set FAIL=0

call :build "E:\UG\NX12_NXOpenCPP_Wizard1\NX12_MultiModule.sln" || set FAIL=1
call :build "E:\UG\NX12_NXOpenCPP_打标图\NX12_NXOpenCPP_打标图.sln" || set FAIL=1
call :build "E:\UG\NX12_NXOpenCPP_爆炸图\NX12_NXOpenCPP_爆炸图.sln" || set FAIL=1
call :build "E:\UG\NX12_NXOpenCPP_爆炸参数\NX12_NXOpenCPP_爆炸参数.sln" || set FAIL=1
call :build "E:\UG\NX12_NXOpenCPP_球标\NX12_NXOpenCPP_球标.sln" || set FAIL=1

if "%FAIL%"=="1" goto :buildfail

echo.
echo ============================================================
echo  Deploy DLLs to %DEPLOY%
echo ============================================================

set SRC_WIZ=E:\UG\NX12_NXOpenCPP_Wizard1\bin\Release

for %%F in (NX12_Step1_SheetAndViews NX12_Step2_SheetPreferences NX12_Step3_OrdinateDimensions NX12_Step4_LinearDimensions NX12_Step5_Centerlines NX12_Step6_LayerSwitch NX12_Step7_AppendSuffix titleblock_fill) do copy /Y "%SRC_WIZ%\%%F.dll" "%DEPLOY%\" >nul && echo   OK %%F.dll

copy /Y "E:\UG\NX12_NXOpenCPP_打标图\x64\Release\dbt_step1.dll"         "%DEPLOY%\" >nul && echo   OK dbt_step1.dll
copy /Y "E:\UG\NX12_NXOpenCPP_爆炸图\x64\Release\explosion_step1.dll"   "%DEPLOY%\" >nul && echo   OK explosion_step1.dll
copy /Y "E:\UG\NX12_NXOpenCPP_爆炸参数\x64\Release\explosion_step2.dll" "%DEPLOY%\" >nul && echo   OK explosion_step2.dll
copy /Y "E:\UG\NX12_NXOpenCPP_球标\bin\Release\balloon_step1.dll"       "%DEPLOY%\" >nul && echo   OK balloon_step1.dll

echo.
echo ============================================================
echo  Done. Restart NX12 and look for the twp toolbox cascade
echo  (main menu bar, left of Help) and the Applications tab.
echo ============================================================
exit /b 0

:nomsbuild
echo [ERROR] MSBuild not found: "%MSBUILD%"
exit /b 1

:buildfail
echo.
echo [FAILED] Some projects failed to build. Deploy skipped.
exit /b 1

:build
echo.
echo ----- Building: %~1 -----
"%MSBUILD%" "%~1" /p:Configuration=Release /p:Platform=x64 /p:UGII_BASE_DIR="%UGII_BASE_DIR%" /m /v:minimal /nologo
exit /b %ERRORLEVEL%
