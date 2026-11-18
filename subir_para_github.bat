@echo off
title Enviar Resident Evil Degeneration para o GitHub
cd /d "%~dp0"
echo ========================================================
echo   Enviando arquivos para o GitHub
echo   Repositorio: https://github.com/piecarlos/project_degeneration
echo ========================================================
echo.

git push -u origin main

echo.
if %ERRORLEVEL% equ 0 (
    echo [SUCESSO] Projeto enviado com sucesso para o GitHub!
) else (
    echo [ATENCAO] Ocorreu um erro no envio. Verifique a autenticacao acima.
)
echo.
pause
