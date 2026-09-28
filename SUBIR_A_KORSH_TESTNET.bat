@echo off
chcp 65001 > nul
cls
echo ======================================================================
echo       SUBIDA DE CÓDIGO v0.0.5 A: Korsh-Dev/Korsh-Testnet
echo ======================================================================
echo.
echo Repositorio local: %CD%
echo Repositorio remoto: https://github.com/Korsh-Dev/Korsh-Testnet
echo Rama a desplegar: main (con todas las correcciones v0.0.5 y diseno)
echo.
echo Últimos commits a subir:
git log -n 3 --oneline
echo.
echo ======================================================================
echo Si tienes un Personal Access Token (PAT) de GitHub, puedes pegarlo abajo.
echo Si NO tienes token, solo presiona ENTER y se abrira tu navegador para
echo autorizar con un solo clic.
echo ======================================================================
echo.
set /p GITHUB_TOKEN="Token de GitHub (opcional, ENTER para navegador): "

if "%GITHUB_TOKEN%"=="" (
    echo.
    echo Iniciando autenticación mediante navegador / Git Credential Manager...
    git push testnet main:main --force
) else (
    echo.
    echo Subiendo mediante Token de acceso personal...
    git push https://%GITHUB_TOKEN%@github.com/Korsh-Dev/Korsh-Testnet.git main:main --force
)

if %ERRORLEVEL% equ 0 (
    echo.
    echo ======================================================================
    echo ¡ÉXITO! Todo el código v0.0.5 ha sido subido a Korsh-Dev/Korsh-Testnet.
    echo Enlace: https://github.com/Korsh-Dev/Korsh-Testnet
    echo ======================================================================
) else (
    echo.
    echo ======================================================================
    echo Ocurrió un error al subir. Verifica tus credenciales o permisos en el repo.
    echo ======================================================================
)

echo.
pause
