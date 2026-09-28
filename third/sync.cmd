@echo off
rem ---------------------------------------------------------------------------
rem The external checkouts this repo builds against: clone them, report on them,
rem or move them forward. manifest.txt is the record; this reads it.
rem
rem   sync.cmd [check^|clone^|update^|verify] [name ...]
rem
rem     check    (default) say what is there, whether it is on the pinned
rem              commit, whether the patches are applied, whether anything else
rem              is in the tree. Changes nothing.
rem     clone    clone whatever is missing at its pinned commit, init
rem              submodules, apply patches. Existing checkouts are left alone.
rem     update   move each checkout to its branch tip and rewrite manifest.txt
rem              with the new pins. Patches come off first and go back on after,
rem              so one that no longer fits STOPS that dependency rather than
rem              leaving it half-moved.
rem     verify   for the build scripts: silent and exit 0 when each checkout is
rem              on its pinned commit with every patch applied; otherwise one
rem              line per problem on stderr and exit 1. Changes nothing. A
rem              missing checkout counts as a problem. Stray files do not -- a
rem              patched tree always has some, so they are check's business.
rem
rem   name ...   limit to these (manifest names, case-insensitive). No name
rem              means all of them.
rem
rem Checkouts are siblings of this repo, not children of it. That is not taste:
rem they are multi-gigabyte trees carrying their own build output, and one
rem `git clean -xdf` in here would delete them. Each one's location can be
rem overridden by the same environment variable the build scripts already read
rem -- LLAMA_SRC, MNN_SRC, QTTS_SRC, VCPKG_ROOT -- so this script and the builds
rem cannot end up pointing at different trees.
rem
rem Env overrides:
rem   THIRD_ROOT   where checkouts live (default: this repo's parent directory)
rem   <dep>_SRC    one checkout's exact path; see the env column in manifest.txt
rem   Each <dep>_SRC also reads as YOAI_<dep>_SRC, from the environment or from
rem   .env at the repository root -- the same loader the build scripts call.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Before any default below, so a path set in .env is the one cloned into.
rem No names: a clone target does not exist yet, and a missing checkout is what
rem check and verify report.
call "%~dp0..\_load-env.cmd"

set "MANIFEST=%~dp0manifest.txt"
set "PATCHDIR=%~dp0patches"
if not defined THIRD_ROOT for %%I in ("%~dp0..\..") do set "THIRD_ROOT=%%~fI"

if not exist "!MANIFEST!" (
    echo no manifest at "!MANIFEST!".>&2
    exit /b 1
)
where git >nul 2>&1 || (
    echo need git on PATH.>&2
    exit /b 1
)

rem --- Arguments, position-independent ---------------------------------------
rem One mode keyword plus any number of dependency names. An unrecognized token
rem is an error, not a guess: quietly doing `check` because somebody typed
rem `updte` is the failure worth designing out.
rem
rem ONLY accumulates as ,name,name, -- commas rather than spaces because `echo`
rem eats one leading space, which would make the first name unmatchable.
rem `exit /b 1` from inside a for body ends the script but loses the code, so
rem every loop in this file records a flag and the caller acts on it after.
set "MODE=check"
set "ONLY="
set "BADARG="
for %%A in (%*) do (
    set "TOK=%%~A"
    set "HANDLED="
    if /I "!TOK!"=="check"  set "MODE=check"  & set "HANDLED=1"
    if /I "!TOK!"=="clone"  set "MODE=clone"  & set "HANDLED=1"
    if /I "!TOK!"=="update" set "MODE=update" & set "HANDLED=1"
    if /I "!TOK!"=="verify" set "MODE=verify" & set "HANDLED=1"
    if not defined HANDLED (
        findstr /B /I /C:"!TOK!|" "!MANIFEST!" >nul
        if errorlevel 1 (
            echo unknown argument: !TOK!>&2
            set "BADARG=1"
        ) else (
            if not defined ONLY set "ONLY=,"
            set "ONLY=!ONLY!!TOK!,"
        )
    )
)
if defined BADARG (
    echo   modes: check ^| clone ^| update ^| verify>&2
    echo   names: the first column of manifest.txt>&2
    exit /b 1
)

if /I not "!MODE!"=="verify" (
    echo [third] root=!THIRD_ROOT!  mode=!MODE!
    echo.
)

rem --- The table -------------------------------------------------------------
rem Read once into DEP_n_* so `update` can re-emit manifest.txt from it at the
rem end. eol=# drops the header; delims=^| splits the columns. An empty column
rem is written `-` rather than left blank because for /f collapses consecutive
rem delimiters and would shift every token after it.
set /a NDEP=0
set "FAILED="
for /f "usebackq eol=# tokens=1-8 delims=|" %%A in ("!MANIFEST!") do (
    set /a NDEP+=1
    set "DEP_!NDEP!_NAME=%%A"
    set "DEP_!NDEP!_ENV=%%B"
    set "DEP_!NDEP!_URL=%%C"
    set "DEP_!NDEP!_BRANCH=%%D"
    set "DEP_!NDEP!_COMMIT=%%E"
    set "DEP_!NDEP!_SUB=%%F"
    set "DEP_!NDEP!_PATCHES=%%G"
    set "DEP_!NDEP!_USEDBY=%%H"
)
if !NDEP! EQU 0 (
    echo manifest.txt has no entries.>&2
    exit /b 1
)

for /L %%I in (1,1,!NDEP!) do call :dep %%I

if /I "!MODE!"=="update" call :rewrite
rem verify says nothing on success and has already named each problem. At the
rem top level rather than in an if block: `cmd /c sync.cmd` drops an exit
rem code set from inside one, and a caller outside a batch file sees 0.
if /I not "!MODE!"=="verify" goto verify_done
if defined FAILED exit /b 1
exit /b 0
:verify_done
if defined FAILED (
    echo.
    echo [third] trouble with:!FAILED!
    exit /b 1
)
echo.
echo [third] done.
exit /b 0

rem ===========================================================================
rem Read one variable indirectly: %1 is the name to set, %2 the name to read.
rem
rem `!%~2!` works because cmd expands %-parameters BEFORE it expands !vars!, so
rem by the time delayed expansion runs the line already says !DEP_1_NAME!. The
rem obvious-looking alternatives (`call set`, `%%VAR%%`) either re-parse the
rem value or collide with for-variable substitution.
:getvar
set "%~1=!%~2!"
exit /b 0

rem ===========================================================================
rem One dependency. %1 is its index in the table.
:dep
set "IDX=%~1"
for %%V in (NAME ENV URL BRANCH COMMIT SUB PATCHES USEDBY) do call :getvar D_%%V DEP_!IDX!_%%V
if defined ONLY (
    echo !ONLY!| findstr /I /C:",!D_NAME!," >nul
    if errorlevel 1 exit /b 0
)

rem Where it lives: the build scripts' own variable wins, so the two can never
rem disagree about which tree is being built.
call :getvar DIR !D_ENV!
if not defined DIR set "DIR=!THIRD_ROOT!\!D_NAME!"

if /I "!MODE!"=="verify" (
    if not exist "!DIR!\.git" (
        echo [third] !D_NAME!: MISSING at !DIR!>&2
        set "FAILED=!FAILED! !D_NAME!"
    ) else (
        call :verify
        if errorlevel 1 set "FAILED=!FAILED! !D_NAME!"
    )
    exit /b 0
)

echo === !D_NAME!  ^(!D_USEDBY!^)
echo     !DIR!

if not exist "!DIR!\.git" (
    if /I not "!MODE!"=="clone" (
        echo     MISSING -- `sync.cmd clone !D_NAME!` would fetch it
        echo.
        exit /b 0
    )
    call :clone
    if errorlevel 1 (
        set "FAILED=!FAILED! !D_NAME!"
        echo.
        exit /b 0
    )
)

if /I "!MODE!"=="update" (
    call :update
    if errorlevel 1 (
        set "FAILED=!FAILED! !D_NAME!"
        echo.
        exit /b 0
    )
)
call :report
echo.
exit /b 0

rem ---------------------------------------------------------------------------
rem Clone at the pinned commit. Not --depth 1 and not --branch: the pin is a
rem commit rather than a tag, and it is usually behind the tip, so neither
rem shortcut can check it out.
:clone
echo     cloning...
git clone "!D_URL!" "!DIR!" || exit /b 1
git -C "!DIR!" checkout --detach "!D_COMMIT!" || exit /b 1
if /I "!D_SUB!"=="yes" (
    git -C "!DIR!" submodule update --init --recursive || exit /b 1
)
call :patches apply || exit /b 1
exit /b 0

rem ---------------------------------------------------------------------------
rem Move to the branch tip. The patches come off before the fetch: pulling over
rem a patched file either conflicts or silently merges, and neither is a thing
rem to discover during a build three days later.
:update
call :patches reverse || exit /b 1
echo     fetching...
git -C "!DIR!" fetch origin "!D_BRANCH!" || exit /b 1
git -C "!DIR!" checkout --detach FETCH_HEAD || exit /b 1
if /I "!D_SUB!"=="yes" (
    git -C "!DIR!" submodule update --init --recursive || exit /b 1
)
for /f "delims=" %%H in ('git -C "!DIR!" rev-parse HEAD') do set "D_COMMIT=%%H"
set "DEP_!IDX!_COMMIT=!D_COMMIT!"
call :patches apply || exit /b 1
exit /b 0

rem ---------------------------------------------------------------------------
rem Every patch for this dependency, in manifest order. %1 is apply or reverse.
rem
rem `--reverse --check` is the idempotence test, and a better one than a marker
rem file: it succeeds only when the change is ALREADY present. So applying twice
rem is a no-op, a checkout that moved under a patch fails loudly, and a file
rem somebody edited by hand is detected rather than patched on top of.
rem PERR rather than `exit /b 1` for the reason given at the argument loop, and
rem it doubles as the guard that stops the remaining patches once one has gone
rem wrong -- half-patching a tree is worse than not touching it.
:patches
set "ACT=%~1"
set "PERR="
if "!D_PATCHES!"=="-" exit /b 0
for %%Q in ("!D_PATCHES:,=" "!") do if not defined PERR (
    for /f "tokens=1,2 delims=@" %%F in ("%%~Q") do (
        set "PFILE=!PATCHDIR!\%%F"
        set "PTREE=!DIR!"
        if not "%%G"=="." set "PTREE=!DIR!\%%G"
        if not exist "!PFILE!" (
            echo     no patch at "!PFILE!".>&2
            set "PERR=1"
        ) else (
            git -C "!PTREE!" apply --reverse --check "!PFILE!" >nul 2>&1
            if errorlevel 1 (set "APPLIED=") else (set "APPLIED=1")
            if /I "!ACT!"=="apply" (
                if defined APPLIED (
                    echo     patch ok: %%F
                ) else (
                    git -C "!PTREE!" apply "!PFILE!"
                    if errorlevel 1 (
                        echo     COULD NOT APPLY %%F in "!PTREE!" -- rebase it against the new tree.>&2
                        set "PERR=1"
                    ) else (
                        echo     patched: %%F
                    )
                )
            ) else (
                if defined APPLIED (
                    git -C "!PTREE!" apply --reverse "!PFILE!"
                    if errorlevel 1 (
                        echo     could not reverse %%F in "!PTREE!".>&2
                        set "PERR=1"
                    ) else (
                        echo     un-patched: %%F
                    )
                )
            )
        )
    )
)
if defined PERR exit /b 1
exit /b 0

rem ---------------------------------------------------------------------------
rem The same two facts :report states, said only when they are wrong, so a
rem build can run this every time and print nothing on the normal path.
:verify
set "VBAD="
for /f "delims=" %%H in ('git -C "!DIR!" rev-parse HEAD') do set "HEAD=%%H"
if /I not "!HEAD!"=="!D_COMMIT!" (
    echo [third] !D_NAME!: commit !HEAD:~0,12!, manifest pins !D_COMMIT:~0,12!>&2
    set "VBAD=1"
)
if not "!D_PATCHES!"=="-" (
    for %%Q in ("!D_PATCHES:,=" "!") do (
        for /f "tokens=1,2 delims=@" %%F in ("%%~Q") do (
            set "PTREE=!DIR!"
            if not "%%G"=="." set "PTREE=!DIR!\%%G"
            git -C "!PTREE!" apply --reverse --check "!PATCHDIR!\%%F" >nul 2>&1
            if errorlevel 1 (
                echo [third] !D_NAME!: patch MISSING: %%F>&2
                set "VBAD=1"
            )
        )
    )
)
if defined VBAD exit /b 1
exit /b 0

rem ---------------------------------------------------------------------------
rem What is there, against what the manifest says. Three facts reported
rem separately because they go wrong separately: the commit, the patches, and
rem whatever else somebody left in the tree.
:report
for /f "delims=" %%H in ('git -C "!DIR!" rev-parse HEAD') do set "HEAD=%%H"
if /I "!HEAD!"=="!D_COMMIT!" (
    echo     commit !HEAD:~0,12! ^(pinned^)
) else (
    echo     commit !HEAD:~0,12! -- manifest pins !D_COMMIT:~0,12!
)
if not "!D_PATCHES!"=="-" (
    for %%Q in ("!D_PATCHES:,=" "!") do (
        for /f "tokens=1,2 delims=@" %%F in ("%%~Q") do (
            set "PTREE=!DIR!"
            if not "%%G"=="." set "PTREE=!DIR!\%%G"
            git -C "!PTREE!" apply --reverse --check "!PATCHDIR!\%%F" >nul 2>&1
            if errorlevel 1 (echo     patch MISSING: %%F) else (echo     patch ok: %%F)
        )
    )
)
rem Anything beyond the patches. A patched file is EXPECTED to show as modified,
rem so this counts paths instead of asserting a clean tree: it is a prompt to
rem look, not a failure.
set /a NDIRTY=0
for /f "delims=" %%L in ('git -C "!DIR!" status --porcelain') do set /a NDIRTY+=1
if !NDIRTY! GTR 0 echo     !NDIRTY! modified/untracked path^(s^) -- git -C "!DIR!" status
exit /b 0

rem ---------------------------------------------------------------------------
rem Re-emit manifest.txt from the table, carrying whatever `update` moved. Only
rem the commit column ever changes, but re-emitting whole lines keeps one code
rem path instead of a search-and-replace that has to know the column layout.
:rewrite
set "TMP=%TEMP%\third-manifest-%RANDOM%.txt"
> "!TMP!" echo # name^|env^|url^|branch^|commit^|submodules^|patches ^(file@worktree^)^|used by
for /L %%I in (1,1,!NDEP!) do (
    for %%V in (NAME ENV URL BRANCH COMMIT SUB PATCHES USEDBY) do call :getvar R_%%V DEP_%%I_%%V
    >> "!TMP!" echo !R_NAME!^|!R_ENV!^|!R_URL!^|!R_BRANCH!^|!R_COMMIT!^|!R_SUB!^|!R_PATCHES!^|!R_USEDBY!
)
move /y "!TMP!" "!MANIFEST!" >nul
echo.
echo [third] manifest.txt rewritten with the new pins -- git diff third/manifest.txt
exit /b 0
