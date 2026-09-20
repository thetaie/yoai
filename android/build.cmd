@echo off
rem ---------------------------------------------------------------------------
rem Windows entry point for android/build.sh.
rem
rem A THIN WRAPPER, unlike harmony/build.cmd, which is that lane's canonical
rem script with build.sh mirroring it. The reason is the toolchain: the OHOS
rem lane drives DevEco's own cmake/ninja/hvigor through Windows paths and a
rem native batch file is the shorter road. Everything below is the NDK, vcpkg
rem and the Gradle wrapper, which are identical on all three platforms -- so a
rem second copy of three hundred lines would only be two things to keep in
rem agreement.
rem
rem Needs a bash. Git for Windows ships one, and installing the Android SDK
rem without also having git is unusual enough not to plan for.
rem
rem   build.cmd [abi] [deps|llama|qtts|vulkan|apk|all] [clean] [arch=...]
rem
rem See build.sh --help for what the tokens mean.
rem ---------------------------------------------------------------------------
setlocal

set "HERE=%~dp0"

rem PATH first, then the two places Git for Windows installs.
set "BASH="
for %%B in (bash.exe) do if not defined BASH set "BASH=%%~$PATH:B"
if not defined BASH if exist "%ProgramFiles%\Git\bin\bash.exe" set "BASH=%ProgramFiles%\Git\bin\bash.exe"
if not defined BASH if exist "%ProgramFiles(x86)%\Git\bin\bash.exe" set "BASH=%ProgramFiles(x86)%\Git\bin\bash.exe"

if not defined BASH (
    echo need bash ^(Git for Windows^), which android/build.sh runs under.>&2
    echo   https://git-scm.com/download/win>&2
    exit /b 1
)

"%BASH%" "%HERE%build.sh" %*
exit /b %ERRORLEVEL%
