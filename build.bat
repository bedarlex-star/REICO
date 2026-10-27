@echo off
cd /d "%~dp0"
taskkill /im REICO.exe >nul 2>&1
C:\msys64\ucrt64\bin\windres.exe reico.rc -O coff -o reico.res
C:\msys64\ucrt64\bin\g++.exe main.cpp reico.res -municode -mwindows -O2 -s -static -o REICO.exe -lshell32 -lwinhttp
pause
