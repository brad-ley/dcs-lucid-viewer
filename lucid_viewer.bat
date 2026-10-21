@echo off

:: Some locked-down machines launch a shortcut's cmd.exe with a PATH that
:: omits %SystemRoot%\System32 -- which breaks conda's activation hook,
:: since it shells out to doskey.exe (System32\doskey.exe) to register the
:: "conda" command. Prepending it here makes this launcher work regardless
:: of whatever PATH the shortcut started with.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%PATH%"

set "SCRIPT=Z:\6. Software\prod_code\LucidVisionCamera\lucid_viewer.py"
:: Dedicated env (matches environment.yml's own "name:") rather than the
:: shared "dcs-datascripts" env used elsewhere on these machines -- PyQt6 and
:: PyQt6-Qt6 (bindings vs. the actual Qt DLLs) aren't version-locked by pip,
:: so an unrelated install into a shared env can silently push them out of
:: sync and break this viewer with "DLL load failed ... QtWidgets" on launch.
:: A dedicated env can't be perturbed by installs made for other scripts.
set ENV_NAME=lucid-viewer

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

:: ── Activate, don't just invoke python.exe by path ───────────────────────────
:: Conda-forge packages (numpy/OpenBLAS among them) ship activate.d hook
:: scripts that set environment variables Windows DLL loading depends on --
:: notably CONDA_DLL_SEARCH_MODIFICATION_ENABLE, which works around a
:: Windows DLL-search-order change that otherwise silently breaks resolving
:: a conda-forge package's own Library\bin DLLs. Invoking "%PYTHON%" by its
:: full path (as this used to) skips all of that, and was traced to a
:: numpy/OpenBLAS crash (0xc06d007f) during the PCA correction's matrix
:: multiply that only reproduced in a shell where conda activate had never
:: run -- i.e. exactly this launcher's own usual invocation pattern.
CALL "%CONDA%" activate %ENV_NAME%
IF ERRORLEVEL 1 (
    echo ERROR: failed to activate conda environment %ENV_NAME%.
    pause
    exit /b 1
)

:: ── Launch viewer ─────────────────────────────────────────────────────────────
python "%SCRIPT%" %*
