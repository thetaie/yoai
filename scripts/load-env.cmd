@rem Settings for the build scripts, from the environment and from .env.
@rem
@rem CALLED, not run: `call "%~dp0..\scripts\load-env.cmd" NAME ...`. It sets
@rem variables in the caller's environment, so it has no setlocal of its own.
@rem
@rem The NAMEs are the paths the caller READS. Each one that is set by now --
@rem from .env or from the environment, under either name -- must exist, or
@rem this names it and exits 1, so a typo stops the build here rather than
@rem surfacing later as some other tool's error. Paths the caller creates, or
@rem clones into, are left off the list, and a name left unset is the caller's
@rem to default and check.
@rem
@rem Every setting is named YOAI_<NAME> and fills the <NAME> the build scripts
@rem already read. Highest wins:
@rem
@rem   1. YOAI_<NAME> in the environment
@rem   2. YOAI_<NAME> in .env at the repository root
@rem   3. <NAME> in the environment, e.g. the VULKAN_SDK its installer sets
@rem   4. the calling script's own default
@rem
@rem Only YOAI_ lines are taken from .env, so the file can hold other tools'
@rem settings as well. .env.example lists every setting.
@rem
@rem The format is the part of dotenv both shells can read without executing
@rem anything: KEY=VALUE per line, no spaces around `=`, # for comments, and a
@rem value may be wrapped in double quotes. A value containing ! or % is not
@rem supported -- the callers run with delayed expansion on.

@rem A goto rather than an `if exist (...)` block: a ) anywhere in the checkout's
@rem path would close the block early.
@if not exist "%~dp0..\.env" goto yoai_map
@for /f "usebackq tokens=1,* delims==" %%A in (`findstr /b /r /c:"YOAI_[A-Z0-9_]*=" "%~dp0..\.env"`) do @if not defined %%A set "%%A=%%~B"

:yoai_map
@rem One list for every script. A script that does not read a name is not
@rem affected by it being set.
@for %%N in (VS_DIR NINJA VCPKG_ROOT OPENSSL_ROOT_DIR VULKAN_SDK LLAMA_SRC LLAMA_SDK_DIR QTTS_SRC MNN_SRC DEVECO_HOME OHOS_SDK_ROOT) do @if defined YOAI_%%N call set "%%N=%%YOAI_%%N%%"

@set "YOAI_ENV_BAD="
@for %%N in (%*) do @if defined %%N call :yoai_check %%N
@set "YOAI_ENV_VAL="
@if not defined YOAI_ENV_BAD exit /b 0
@set "YOAI_ENV_BAD="
@exit /b 1

@rem One name. The value is read into a variable of its own on one line and used
@rem on the next, because a line is expanded before it runs.
:yoai_check
@call set "YOAI_ENV_VAL=%%%1%%"
@if exist "%YOAI_ENV_VAL%" goto :eof
@echo [env] %1 is "%YOAI_ENV_VAL%", which does not exist. Fix YOAI_%1 in .env, or %1 / YOAI_%1 in the environment.>&2
@set "YOAI_ENV_BAD=1"
@goto :eof
