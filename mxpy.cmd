@echo off
setlocal
if defined MXWRITE_PYTHON (
    "%MXWRITE_PYTHON%" "%~dp0mxpy.py" %*
) else (
    python.exe --version >nul 2>nul
    if errorlevel 1 (
        py "%~dp0mxpy.py" %*
    ) else (
        python.exe "%~dp0mxpy.py" %*
    )
)
exit /b %errorlevel%
