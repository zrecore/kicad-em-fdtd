@echo off
rem emfdtd-farm: runs the GPU farm tool with a Python that has TLS 1.3 (the farm refuses to run without it).
rem Looks for a suitable Python in this order: %EMFDTD_PYTHON%, python, python3, the py launcher (3.13 ... 3.9),
rem then the usual install folders. No network access, nothing is downloaded or installed.
rem   emfdtd-farm --which-python    print the Python it would use and stop
rem   emfdtd-farm ARGS...           the same as: PYTHON emfdtd_farm.py ARGS...
setlocal EnableExtensions
set "HERE=%~dp0"
set "PY="
set "PYARGS="
set "PF86=%ProgramFiles(x86)%"

if defined EMFDTD_PYTHON call :tryfile "%EMFDTD_PYTHON%"
if not defined PY call :try python
if not defined PY call :try python3
for %%V in (3.13 3.12 3.11 3.10 3.9 3) do call :trypy %%V
for %%V in (313 312 311 310 39) do call :trydirs %%V
if not defined PY goto :nopython
if /I "%~1"=="--which-python" goto :which
"%PY%" %PYARGS% "%HERE%emfdtd_farm.py" %*
exit /b %ERRORLEVEL%

:which
echo "%PY%" %PYARGS%
exit /b 0

:try
if defined PY goto :eof
"%~1" -c "import ssl, sys; sys.exit(0 if ssl.HAS_TLSv1_3 and sys.version_info >= (3, 9) else 1)" >nul 2>nul
if not errorlevel 1 set "PY=%~1"
goto :eof

:tryfile
if exist "%~1" call :try "%~1"
goto :eof

:trypy
if defined PY goto :eof
py -%~1 -c "import ssl, sys; sys.exit(0 if ssl.HAS_TLSv1_3 and sys.version_info >= (3, 9) else 1)" >nul 2>nul
if not errorlevel 1 set "PY=py"
if not errorlevel 1 set "PYARGS=-%~1"
goto :eof

:trydirs
call :tryfile "%LocalAppData%\Programs\Python\Python%~1\python.exe"
call :tryfile "%ProgramFiles%\Python%~1\python.exe"
call :tryfile "%PF86%\Python%~1\python.exe"
call :tryfile "C:\Python%~1\python.exe"
goto :eof

:nopython
echo emfdtd-farm: no suitable Python found. 1>&2
echo The farm needs Python 3.9 or newer whose ssl module has TLS 1.3 (OpenSSL 1.1.1 or newer). 1>&2
echo   Install one from https://www.python.org/downloads/windows/ (tick "Add python.exe to PATH"), 1>&2
echo   or run:  winget install Python.Python.3.13 1>&2
echo   Check a Python yourself:  python -c "import ssl; print(ssl.OPENSSL_VERSION, ssl.HAS_TLSv1_3)"   (must print True) 1>&2
echo   Installed somewhere else?  set EMFDTD_PYTHON=C:\full\path\to\python.exe 1>&2
exit /b 3
