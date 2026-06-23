@echo off
setlocal

:: ── Paths ──────────────────────────────────────────────────────────────────────
set "REPO=%~dp0"
:: Strip trailing backslash
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
set "SCRIPT=%REPO%\lucid_viewer.py"
set "ENV_YML=%REPO%\environment.yml"
set "ENV_NAME=lucid-viewer"

:: ── Pull latest from GitHub ────────────────────────────────────────────────────
where git >nul 2>&1
if errorlevel 1 (
    echo WARNING: git not found on PATH — skipping update.
    echo          Install Git from https://git-scm.com/download/win
    goto :skip_git
)
echo Pulling latest code from GitHub...
git -C "%REPO%" pull origin github-branch
if errorlevel 1 (
    echo WARNING: git pull failed. Running with local copy.
)
:skip_git

:: ── Find conda ─────────────────────────────────────────────────────────────────
set CONDA1=%USERPROFILE%\AppData\Local\miniconda3\condabin\conda.bat
set CONDA2=%USERPROFILE%\miniconda3\condabin\conda.bat
set CONDA3=C:\ProgramData\miniconda3\condabin\conda.bat
set CONDA4=C:\ProgramData\Miniconda3\condabin\conda.bat

if exist "%CONDA1%" ( set "CONDA=%CONDA1%" & goto :conda_found )
if exist "%CONDA2%" ( set "CONDA=%CONDA2%" & goto :conda_found )
if exist "%CONDA3%" ( set "CONDA=%CONDA3%" & goto :conda_found )
if exist "%CONDA4%" ( set "CONDA=%CONDA4%" & goto :conda_found )

echo ERROR: Miniconda not found.
echo        Install from https://docs.conda.io/en/latest/miniconda.html
pause
exit /b 1

:conda_found
:: Derive conda root (two levels up from condabin\conda.bat)
for %%i in ("%CONDA%\..\..") do set "CONDA_ROOT=%%~fi"
set "PYTHON=%CONDA_ROOT%\envs\%ENV_NAME%\python.exe"

:: ── Create or update environment ───────────────────────────────────────────────
if not exist "%PYTHON%" (
    echo Creating environment %ENV_NAME% from environment.yml...
    call "%CONDA%" env create -n %ENV_NAME% -f "%ENV_YML%"
    if errorlevel 1 (
        echo ERROR: Failed to create conda environment.
        pause
        exit /b 1
    )
) else (
    echo Updating environment %ENV_NAME%...
    call "%CONDA%" env update -n %ENV_NAME% -f "%ENV_YML%" --prune
)

:: ── Activate environment and launch viewer ──────────────────────────────────────
echo Starting LucidVision viewer...
call "%CONDA%" activate %ENV_NAME%
python "%SCRIPT%" %*
