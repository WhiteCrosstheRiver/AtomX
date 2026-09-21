@echo off
cd /d "%~dp0"
start "" "build\AtomX.exe" --adapter 1 %*
