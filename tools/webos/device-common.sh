#!/usr/bin/env bash
# Shared validation and bounded, noninteractive transport for device helpers.

webos_error() { printf 'error: %s\n' "$*" >&2; exit 2; }
webos_require_value() {
  [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || webos_error "$1 requires a value"
}
webos_uint() {
  local name="$1" value="$2" minimum="${3:-1}" maximum="${4:-86400}"
  [[ "$value" =~ ^[0-9]{1,6}$ ]] || webos_error "$name must be an integer ($minimum..$maximum)"
  (( 10#$value >= minimum && 10#$value <= maximum )) || webos_error "$name must be $minimum..$maximum"
}
webos_app_id() {
  [[ "$1" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || webos_error "invalid app id: $1"
}
webos_require_command() {
  command -v "$1" >/dev/null 2>&1 || { printf 'missing required command: %s\n' "$1" >&2; exit 1; }
}
webos_transport_init() {
  local host="$1"
  [[ -n "$host" && "$host" != -* && "$host" != *[[:space:]]* ]] || webos_error 'a non-option SSH host is required'
  WEBOS_COMMAND_TIMEOUT="${WEBOS_COMMAND_TIMEOUT:-30}"
  webos_uint WEBOS_COMMAND_TIMEOUT "$WEBOS_COMMAND_TIMEOUT"
  webos_require_command timeout
  webos_require_command ssh
  WEBOS_SSH=(ssh -F /dev/null -o BatchMode=yes -o ConnectTimeout=10
    -o ConnectionAttempts=1 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)
  WEBOS_SCP=(scp -F /dev/null -q -o BatchMode=yes -o ConnectTimeout=10
    -o ConnectionAttempts=1 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)
}
webos_timeout() { timeout --signal=TERM --kill-after=5s "${WEBOS_COMMAND_TIMEOUT}s" "$@"; }
webos_ssh() { webos_timeout "${WEBOS_SSH[@]}" "$@"; }
webos_scp() { webos_timeout "${WEBOS_SCP[@]}" -- "$@"; }
# Quote one argument for the remote POSIX shell (not Bash's locale-dependent %q).
webos_quote() { printf "'%s'" "${1//\'/\'\\\'\'}"; }
webos_luna() {
  local host="$1" uri="$2" payload="${3:-\{\}}" bin="${LUNA_BIN:-luna-send}"
  [[ "$bin" =~ ^[a-zA-Z0-9_./-]+$ && "$bin" != -* ]] || webos_error 'invalid LUNA_BIN'
  webos_ssh -n "$host" "$bin -n 1 $(webos_quote "$uri") $(webos_quote "$payload")"
}
