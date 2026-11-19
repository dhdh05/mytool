@echo off
rem Build TaskbarStats.exe - tu chon MSVC (Developer Command Prompt) hoac MinGW-w64 (gcc trong PATH)
where cl >nul 2>nul
if %errorlevel%==0 (
    cl /nologo /O1 /W3 /utf-8 /DUNICODE /D_UNICODE taskbarstats.c /Fe:TaskbarStats.exe /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF
    goto done
)
where gcc >nul 2>nul
if %errorlevel%==0 (
    gcc -municode -mwindows -Os -s -Wall -ffunction-sections -fdata-sections -Wl,--gc-sections ^
        -o TaskbarStats.exe taskbarstats.c -lpdh -ldxgi -ldxguid -lgdi32 -luser32 -ladvapi32 -lsetupapi
    goto done
)
echo Khong tim thay cl.exe hoac gcc. Mo "x64 Native Tools Command Prompt for VS" hoac cai MinGW-w64 (MSYS2).
exit /b 1
:done
if exist TaskbarStats.exe (for %%F in (TaskbarStats.exe) do echo Xong: TaskbarStats.exe - %%~zF bytes)
