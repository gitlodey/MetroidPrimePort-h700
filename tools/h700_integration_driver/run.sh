#!/bin/sh
# Explicit diagnostic entry only; this file never discovers a game/user directory.
set -eu
umask 077
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
case "${1:-}" in
  --help)
    [ "$#" -eq 1 ] || exit 2
    printf '%s\n' 'H700 integration: NOT device-validated. Separate IG14 permission required.' \
      'Usage: ./run.sh --run /absolute/fresh-test-directory' \
      'App deadline 60 seconds; external timeout -s KILL 90 seconds. Timeout is FAIL, not cleanup proof.'
    exit 0 ;;
  --version)
    [ "$#" -eq 1 ] || exit 2
    exec "$HERE/h700_integration_driver" --version ;;
esac
[ "$#" -eq 2 ] && [ "$1" = --run ] || { echo 'Explicit --run /absolute/fresh-test-directory required' >&2; exit 2; }
TEST=$2
case "$TEST" in /*) ;; *) echo 'Absolute fresh test path required' >&2; exit 2 ;; esac
[ "$TEST" != / ] && [ ! -e "$TEST" ] && [ ! -L "$TEST" ] && [ -d "$(dirname -- "$TEST")" ] || {
  echo 'Fresh non-root test path with an existing parent required; nothing removed' >&2; exit 2;
}
[ -z "${LD_LIBRARY_PATH:-}" ] && [ -z "${LD_PRELOAD:-}" ] || {
  echo 'Conflicting loader overrides; refusing altered runtime providers' >&2; exit 2;
}
command -v timeout >/dev/null 2>&1 || { echo 'Required external timeout utility is unavailable' >&2; exit 2; }
export SDL_VIDEODRIVER=sdl2 SDL3SHIM_SDL2_VIDEODRIVER=mali
# GNU/coreutils and BusyBox timeout accept these options. No TERM grace extends the 90 s bound.
# No cleanup/success message follows timeout; the driver alone must prove both generation releases.
exec timeout -s KILL 90 "$HERE/h700_integration_driver" --run --test-dir "$TEST"
