@echo off

:: Some locked-down machines launch a shortcut's cmd.exe with a PATH that
:: omits %SystemRoot%\System32 -- which breaks conda's activation hook,
:: since it shells out to doskey.exe (System32\doskey.exe) to register the
:: "conda" command. Prepending it here makes this launcher work regardless
:: of whatever PATH the shortcut started with.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%PATH%"

:: ── Self-install a Desktop shortcut, once ────────────────────────────────────
::
:: %~f0/%~dp0 resolve to wherever THIS .bat is actually running from, so the
:: shortcut points at the right copy whether that's the dev tree or the
:: prod_code deployment -- no path needs hardcoding here. Only created if
:: missing, so deleting it sticks and re-running the launcher doesn't fight
:: the user over it.
set "SELF=%~f0"
set "SELFDIR=%~dp0"
set "PS1=%TEMP%\pci_viewer_shortcut.ps1"
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"

:: Explorer resolves and caches a shortcut's icon at render time (e.g. right
:: after login), which can race ahead of the network redirector reconnecting
:: a mapped drive on locked-down machines -- leaving the icon blank forever,
:: even though the drive is available by the time you actually launch it.
:: Same fix dcs-batch.bat's makeDesktopShortcutToBat.bat uses: copy the icon
:: to a local, always-available path and point the shortcut at that instead
:: of the network share.
set "LOCAL_ICON_DIR=C:\ProgramData\PCIViewer"
set "LOCAL_ICON=%LOCAL_ICON_DIR%\tv_png.ico"
IF NOT EXIST "%LOCAL_ICON_DIR%" MKDIR "%LOCAL_ICON_DIR%"
IF NOT EXIST "%LOCAL_ICON%" (
    echo Copying icon to local directory...
    COPY /Y "%SELFDIR%assets\tv_png.ico" "%LOCAL_ICON%" >nul
)

> "%PS1%"  echo $ws = New-Object -ComObject WScript.Shell
>>"%PS1%"  echo $link = Join-Path $ws.SpecialFolders('Desktop') 'Single-frame PCI Viewer.lnk'
>>"%PS1%"  echo if (-not (Test-Path $link)) {
>>"%PS1%"  echo     $sc = $ws.CreateShortcut($link)
>>"%PS1%"  echo     $sc.TargetPath = '%SELF%'
>>"%PS1%"  echo     $sc.WorkingDirectory = '%SELFDIR%'
>>"%PS1%"  echo     $sc.IconLocation = '%LOCAL_ICON%,0'
>>"%PS1%"  echo     $sc.Save()
>>"%PS1%"  echo }

IF EXIST "%POWERSHELL%" (
    echo Checking for desktop shortcut...
    "%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
)
DEL "%PS1%" >nul 2>&1

set "SCRIPT=Z:\6. Software\prod_code\LucidVisionCamera\lucid_viewer.py"
:: Dedicated env (matches environment.yml's own "name:") rather than the
:: shared "dcs-datascripts" env used elsewhere on these machines -- PyQt6 and
:: PyQt6-Qt6 (bindings vs. the actual Qt DLLs) aren't version-locked by pip,
:: so an unrelated install into a shared env can silently push them out of
:: sync and break this viewer with "DLL load failed ... QtWidgets" on launch.
:: A dedicated env can't be perturbed by installs made for other scripts.
set ENV_NAME=lucid-viewer
set GIT_BRANCH=master

cd /d "Z:\6. Software\prod_code\LucidVisionCamera"

:: ── Pull latest changes ──────────────────────────────────────────────────────
set BASH1="%USERPROFILE%\AppData\Local\Programs\Git\bin\bash.exe"
set BASH2="C:\Program Files\Git\bin\bash.exe"

IF EXIST %BASH1% (
    set "BASH=%BASH1%"
) ELSE IF EXIST %BASH2% (
    set "BASH=%BASH2%"
) ELSE (
    set "BASH="
)

IF DEFINED BASH (
    echo Pulling latest changes...
    %BASH% -l -c "cd '/z/6. Software/prod_code/LucidVisionCamera' && git checkout %GIT_BRANCH% && git pull origin %GIT_BRANCH%"
) ELSE (
    echo Git Bash not found -- skipping update check, using code already on disk.
)

:: ── Find conda ────────────────────────────────────────────────────────────────
echo Looking for conda...
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
echo Found conda: %CONDA%
:: Derive conda root (two levels up from condabin\conda.bat)
for %%i in ("%CONDA%\..\..") do set "CONDA_ROOT=%%~fi"
set "PYTHON=%CONDA_ROOT%\envs\%ENV_NAME%\python.exe"

:: ── Create environment if missing, update it if environment.yml changed ───────
:: The stamp is a copy of the environment.yml last applied to this PC's env, so
:: new dependencies reach existing installs without a slow update every launch.
set "ENV_YML=Z:\6. Software\prod_code\LucidVisionCamera\environment.yml"
set "ENV_STAMP=%CONDA_ROOT%\envs\%ENV_NAME%\.lucid_viewer_environment.yml"
IF NOT EXIST "%PYTHON%" (
    echo Environment %ENV_NAME% not found. Creating from environment.yml, this may take a few minutes...
    CALL "%CONDA%" env create -n %ENV_NAME% -f "%ENV_YML%"
    IF NOT ERRORLEVEL 1 copy /y "%ENV_YML%" "%ENV_STAMP%" >nul
    echo Environment %ENV_NAME% created.
) ELSE (
    fc /b "%ENV_YML%" "%ENV_STAMP%" >nul 2>&1
    IF ERRORLEVEL 1 (
        echo environment.yml changed -- updating %ENV_NAME%, this may take several minutes...
        CALL "%CONDA%" env update -n %ENV_NAME% -f "%ENV_YML%"
        IF NOT ERRORLEVEL 1 copy /y "%ENV_YML%" "%ENV_STAMP%" >nul
    ) ELSE (
        echo Using existing environment: %ENV_NAME%
    )
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
echo Launching PCI viewer...
python "%SCRIPT%" %*
