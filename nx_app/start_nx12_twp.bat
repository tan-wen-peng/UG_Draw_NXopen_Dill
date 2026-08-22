@echo off
rem twp toolbox launcher - explicit env, bypasses explorer env cache
set "UGII_BASE_DIR=D:\Program Files\Siemens\NX 12.0"
set "UGII_ROOT_DIR=D:\Program Files\Siemens\NX 12.0\UGII"
set "UGII_CUSTOM_DIRECTORY_FILE=E:\UG\nx_app\twp_custom_dirs.dat"
start "" "D:\Program Files\Siemens\NX 12.0\NXBIN\ugraf.exe"
