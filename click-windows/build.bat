@echo off
setlocal
echo ==== Click Window Counter: build helper ====
echo.

where geode >nul 2>nul
if errorlevel 1 (
  echo [!] The Geode command-line tool was not found.
  echo     Install it with:   winget install GeodeSDK.GeodeCLI
  echo     Then CLOSE this window, open a new one, and run build.bat again.
  echo.
  pause
  exit /b 1
)

if "%GEODE_SDK%"=="" (
  echo [!] The Geode SDK is not set up yet.
  echo     Run these two commands, then open a NEW window and run build.bat again:
  echo       geode sdk install
  echo       geode sdk install-binaries
  echo.
  pause
  exit /b 1
)

findstr /c:"yourname.click-windows" mod.json >nul
if not errorlevel 1 (
  echo [!] Open mod.json in Notepad first and change:
  echo       "id"        ^(currently yourname.click-windows^)  to something unique, e.g. mynick.click-windows
  echo       "developer" ^(currently YourName^)               to your name
  echo.
  pause
  exit /b 1
)

echo Building... ^(the first build downloads libraries, so you need to be online^)
echo.
geode build
if errorlevel 1 (
  echo.
  echo First attempt failed, trying again with Ninja...
  echo.
  geode build --ninja
)
if errorlevel 1 (
  echo.
  echo [X] The build failed. Copy the error text above and send it to me.
  echo.
  pause
  exit /b 1
)

echo.
echo [OK] Build finished.
echo If you ran "geode config setup" the mod is already installed in your game.
echo Otherwise copy the .geode file from the "build" folder into your game's geode\mods folder.
echo.
pause
