@echo off
set "DIR=%~dp0"

if exist "%DIR%assets" (
  set "ASSETS=%DIR%assets"
) else (
  set "ASSETS=%DIR%..\..\..\assets"
)

set ARGS=--game_data_root "%ASSETS%" --gpu_plugin=xenos --license_mask=1

if exist "%DIR%update" (
  set ARGS=%ARGS% --update_data_root "%DIR%update"
) else if exist "%DIR%..\..\..\update" (
  set ARGS=%ARGS% --update_data_root "%DIR%..\..\..\update"
)

if exist "%DIR%mods" (
  set ARGS=%ARGS% --mods_data_root "%DIR%mods"
) else if exist "%DIR%..\..\..\mods" (
  set ARGS=%ARGS% --mods_data_root "%DIR%..\..\..\mods"
)

"%DIR%{{EXE}}" %ARGS% %*
