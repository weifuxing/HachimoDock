@echo off
setlocal DisableDelayedExpansion
set "workspace=%USERPROFILE%\.claw-pet\authoring"
if exist "%workspace%\desktop-path" set /p "desktop=" < "%workspace%\desktop-path"
if not defined desktop goto missing
if not exist "%desktop%" goto missing
"%desktop%" --petui %*
exit /b %errorlevel%
:missing
echo Open Component Center ^> Create Component in the current client to update PetUI. 1>&2
exit /b 1
