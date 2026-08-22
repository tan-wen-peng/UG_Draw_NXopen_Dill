@echo off
REM ============================================================
REM NX12 多模块插件一键构建脚本
REM 构建所有 DLL 到统一输出目录
REM ============================================================

set MSBUILD="D:\Program Files (x86)\Microsoft Visual Studio\2017\Community\MSBuild\15.0\Bin\MSBuild.exe"
set CONFIG=Release
set PLATFORM=x64
set SOLUTION=%~dp0NX12_MultiModule.sln

echo ========================================
echo NX12 多模块插件构建
echo 配置: %CONFIG% %PLATFORM%
echo ========================================

%MSBUILD% "%SOLUTION%" /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /m /v:minimal

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo 构建成功！
    echo DLL 输出目录: %~dp0bin\Release\
    echo ========================================
) else (
    echo.
    echo ========================================
    echo 构建失败，请检查以上错误信息
    echo ========================================
    exit /b 1
)
