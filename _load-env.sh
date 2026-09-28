# Settings for the build scripts, from the environment and from .env.
#
# SOURCED, then called: `. <repo>/_load-env.sh && yoai_load_env NAME ...` --
# the leading underscore marks it as something the build scripts call, not
# something to run. It exports variables into the caller's shell. Bash, like
# the scripts that use it.
#
# The NAMEs are the paths the caller READS. Each one that is set by now -- from
# .env or from the environment, under either name -- must exist, or this names
# it and returns 1, so a typo stops the build here rather than surfacing later
# as some other tool's error. Paths the caller creates, or clones into, are left
# off the list, and a name left unset is the caller's to default and check.
#
# Every setting is named YOAI_<NAME> and fills the <NAME> the build scripts
# already read. Highest wins:
#
#   1. YOAI_<NAME> in the environment
#   2. YOAI_<NAME> in .env at the repository root
#   3. <NAME> in the environment, e.g. the VULKAN_SDK its installer sets
#   4. the calling script's own default
#
# Only YOAI_ lines are taken from .env, so the file can hold other tools'
# settings as well. .env.example lists every setting.
#
# The file is READ, never sourced: KEY=VALUE per line, no spaces around `=`,
# # for comments, and a value may be wrapped in double quotes. Nothing in it is
# expanded or executed, which is also what keeps it readable by build.cmd.

yoai_load_env() {
  local root env_file line key val name var
  # The repository root, where this file and .env both live.
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  env_file="$root/.env"

  if [ -f "$env_file" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      line="${line%$'\r'}"   # a .env saved on Windows
      [[ "$line" =~ ^YOAI_[A-Z0-9_]+= ]] || continue
      key="${line%%=*}"
      val="${line#*=}"
      if [[ "$val" == \"*\" ]] && [ "${#val}" -ge 2 ]; then
        val="${val:1:${#val}-2}"
      fi
      if [ -z "${!key+x}" ]; then
        export "$key=$val"
      fi
    done < "$env_file"
  fi

  # One list for every script. A script that does not read a name is not
  # affected by it being set.
  for name in VS_DIR NINJA VCPKG_ROOT OPENSSL_ROOT_DIR VULKAN_SDK LLAMA_SRC \
              LLAMA_SDK_DIR QTTS_SRC MNN_SRC DEVECO_HOME OHOS_SDK_ROOT; do
    var="YOAI_$name"
    if [ -n "${!var:-}" ]; then
      export "$name=${!var}"
    fi
  done

  local bad=0
  for name in "$@"; do
    [ -n "${!name:-}" ] || continue
    if [ ! -e "${!name}" ]; then
      echo "[env] $name is \"${!name}\", which does not exist. Fix YOAI_$name in .env, or $name / YOAI_$name in the environment." >&2
      bad=1
    fi
  done
  return $bad
}
