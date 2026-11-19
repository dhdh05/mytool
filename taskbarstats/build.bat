@echo off
rem Build TaskbarStats.exe - tu chon MSVC (Developer Command Prompt) hoac MinGW-w64 (gcc trong PATH)
rem taskbarstats.rc nhung manifest (Windows 8+) vao exe: bat buoc de gan duoc cua so vao taskbar.
where cl >nul 2>nul
if %errorlevel%==0 (
    rc /nologo taskbarstats.rc || exit /b 1
    cl /nologo /O1 /W3 /utf-8 /DUNICODE /D_UNICODE taskbarstats.c taskbarstats.res /Fe:TaskbarStats.exe ^
        /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO
    del /q taskbarstats.res taskbarstats.obj 2>nul
    goto done
)
where gcc >nul 2>nul
if %errorlevel%==0 (
    windres taskbarstats.rc -O coff -o taskbarstats_res.o || exit /b 1
    gcc -municode -mwindows -Os -s -Wall -ffunction-sections -fdata-sections -Wl,--gc-sections ^
        -o TaskbarStats.exe taskbarstats.c taskbarstats_res.o -lpdh -ldxgi -ldxguid -lgdi32 -luser32 -ladvapi32 -lsetupapi
    del /q taskbarstats_res.o 2>nul
    goto done
)
echo Khong tim thay cl.exe hoac gcc. Mo "x64 Native Tools Command Prompt for VS" hoac cai MinGW-w64 (MSYS2).
exit /b 1
:done
if exist TaskbarStats.exe (for %%F in (TaskbarStats.exe) do echo Xong: TaskbarStats.exe - %%~zF bytes)
