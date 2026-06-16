@echo off

set SCRIPT=lucid_viewer.py
set ENV_NAME=dcs-datascripts
set REPO_URL=https://github.com/brad-ley/dcs-datascripts.git

set DRIVE=Z:
set "REPO_PATH=%DRIVE%\Price\Software\dcs-team\DataScripts"

:: ── Prefer the network copy; fall back to a local clone ───────────────────────
IF EXIST "%REPO_PATH%\%SCRIPT%" (
    cd /d "%REPO_PATH%"
) ELSE (
    echo Network path not found. Cloning repo locally...
    set "REPO_PATH=%USERPROFILE%\dcs-datascripts"
    IF NOT EXIST "%USERPROFILE%\dcs-datascripts" (
        git clone %REPO_URL% "%USERPROFILE%\dcs-datascripts"
    )
    cd /d "%USERPROFILE%\dcs-datascripts"
)

:: ── Find conda ────────────────────────────────────────────────────────────────
set CONDA1=%USERPROFILE%\AppData\Local\miniconda3\condabin\conda.bat
set CONDA2=%USERPROFILE%\miniconda3\condabin\conda.bat
set CONDA3=C:\ProgramData\miniconda3\condabin\conda.bat
set CONDA4=C:\ProgramData\Miniconda3\condabin\conda.bat

IF EXIST "%CONDA1%" ( set "CONDA=%CONDA1%" & goto :conda_found )
IF EXIST "%CONDA2%" ( set "CONDA=%CONDA2%" & goto :conda_found )
IF EXIST "%CONDA3%" ( set "CONDA=%CONDA3%" & goto :conda_found )
IF EXIST "%CONDA4%" ( set "CONDA=%CONDA4%" & goto :conda_found )

echo ERROR: conda not found. Please install Miniconda from https://docs.conda.io/en/latest/miniconda.html
pause
exit /b 1

:conda_found
:: Derive conda root (two levels up from condabin\conda.bat)
for %%i in ("%CONDA%\..\..") do set "CONDA_ROOT=%%~fi"
set "PYTHON=%CONDA_ROOT%\envs\%ENV_NAME%\python.exe"

:: ── Create environment if missing ─────────────────────────────────────────────
IF NOT EXIST "%PYTHON%" (
    echo Environment %ENV_NAME% not found. Creating from environment.yml...
    CALL "%CONDA%" env create -n %ENV_NAME% -f environment.yml
)

:: ── Launch viewer ─────────────────────────────────────────────────────────────
"%PYTHON%" %SCRIPT% %*
