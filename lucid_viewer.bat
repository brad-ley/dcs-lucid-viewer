@echo off

:: Some locked-down machines launch a shortcut's cmd.exe with a PATH that
:: omits %SystemRoot%\System32 -- which breaks conda's activation hook,
:: since it shells out to doskey.exe (System32\doskey.exe) to register the
:: "conda" command. Prepending it here makes this launcher work regardless
:: of whatever PATH the shortcut started with.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%PATH%"

set "SCRIPT=Z:\6. Software\prod_code\LucidVisionCamera\lucid_viewer.py"
set ENV_NAME=dcs-datascripts

cd /d "Z:\6. Software\prod_code\LucidVisionCamera"

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
    CALL "%CONDA%" env create -n %ENV_NAME% -f "Z:\6. Software\prod_code\LucidVisionCamera\environment.yml"
)

:: ── Launch viewer ─────────────────────────────────────────────────────────────
"%PYTHON%" "%SCRIPT%" %*
