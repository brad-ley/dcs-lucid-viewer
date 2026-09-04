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

> "%PS1%"  echo $ws = New-Object -ComObject WScript.Shell
>>"%PS1%"  echo $link = Join-Path $ws.SpecialFolders('Desktop') 'Single-frame PCI Viewer.lnk'
>>"%PS1%"  echo if (-not (Test-Path $link)) {
>>"%PS1%"  echo     $sc = $ws.CreateShortcut($link)
>>"%PS1%"  echo     $sc.TargetPath = '%SELF%'
>>"%PS1%"  echo     $sc.WorkingDirectory = '%SELFDIR%'
>>"%PS1%"  echo     $sc.IconLocation = '%SELFDIR%assets\tv_png.ico,0'
>>"%PS1%"  echo     $sc.Save()
>>"%PS1%"  echo }

IF EXIST "%POWERSHELL%" (
    echo Checking for desktop shortcut...
    "%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
)
DEL "%PS1%" >nul 2>&1

set "SCRIPT=Z:\6. Software\prod_code\LucidVisionCamera\lucid_viewer.py"
set ENV_NAME=dcs-datascripts
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

:: ── Create environment if missing ─────────────────────────────────────────────
IF NOT EXIST "%PYTHON%" (
    echo Environment %ENV_NAME% not found. Creating from environment.yml, this may take a few minutes...
    CALL "%CONDA%" env create -n %ENV_NAME% -f "Z:\6. Software\prod_code\LucidVisionCamera\environment.yml"
    echo Environment %ENV_NAME% created.
) ELSE (
    echo Using existing environment: %ENV_NAME%
)

:: ── Launch viewer ─────────────────────────────────────────────────────────────
echo Launching PCI viewer...
"%PYTHON%" "%SCRIPT%" %*
