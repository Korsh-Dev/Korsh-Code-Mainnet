@echo off
chcp 65001 > nul
cls
echo ======================================================================
echo          DESPLIEGUE A GITHUB: KORSH CORE v0.0.5 (CON BUGS CORREGIDOS)
echo ======================================================================
echo.
echo Rama actual:
git branch --show-current
echo.
echo Últimos commits a desplegar:
git log -n 2 --oneline
echo.
echo Remote de destino actual (origin):
git remote get-url origin
echo.
echo Presiona una tecla para enviar los cambios a GitHub (git push origin main)...
pause > nul
echo.
echo Enviando commits a GitHub...
git push origin main
echo.
echo ¿Deseas también crear y subir el tag v0.0.5? (S/N)
set /p CREAR_TAG="Opción: "
if /i "%CREAR_TAG%"=="S" (
    git tag -a v0.0.5 -m "release: Korsh Core v0.0.5 - consensus fixes, seed redundancy, evo UI fix and obsidian redesign"
    git push origin v0.0.5
    echo Tag v0.0.5 subido con éxito a GitHub.
)
echo.
echo ======================================================================
echo ¡Despliegue finalizado!
echo ======================================================================
pause
