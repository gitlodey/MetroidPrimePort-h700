#!/bin/sh
# Install beside .h700-integration-test/ in ROMS/Ports. Launch from muOS, not over its menu via SSH.
set -eu
umask 077
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
DATA="$HERE/.h700-integration-test"
PACKAGE="$DATA/H700Integration"
case "${1:-}" in
  --help)
    printf '%s\n' 'H700 diagnostic: launch this entry from the muOS Ports menu.' \
      'Same bounded IG14 scenario; app 60 s / watchdog 90 s; physical display acceptance NOT proven.' \
      'Results remain in .h700-integration-test/runs/. No game assets or saves are used.'
    exit 0 ;;
  '') ;;
  *) echo 'No runtime options; launch from muOS Ports' >&2; exit 2 ;;
esac
[ "$#" -eq 0 ] || exit 2
for tool in mktemp sha256sum setsid env; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing prerequisite: $tool" >&2; exit 2; }
done
[ -f "$PACKAGE/SHA256SUMS" ] && [ -f "$PACKAGE/run.sh" ] && [ -f "$DATA/firmware.sha256" ] && [ -d "$DATA/runs" ] || {
  echo 'Diagnostic installation incomplete; nothing launched' >&2; exit 2;
}
mkdir "$DATA/run.lock" 2>/dev/null || { echo 'Diagnostic already running or lock retained; nothing launched' >&2; exit 2; }
trap 'rmdir "$DATA/run.lock" 2>/dev/null || true' EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
RUN=$(mktemp -d "$DATA/runs/run.XXXXXXXX")
printf '%s\n' "$RUN" >"$DATA/latest-run.txt"
exec >"$RUN/launcher.log" 2>&1
printf '%s\n' 'H700 IG14 manual foreground run; physical display acceptance still required.'
sha256sum -c "$DATA/firmware.sha256"
(cd "$PACKAGE" && sha256sum -c SHA256SUMS)
mkdir -m 700 "$RUN/home" "$RUN/config" "$RUN/cache" "$RUN/tmp"
ulimit -c 0
# ext-general.sh supplies these display settings BEFORE invoking the Ports entry.
# Keep only this allowlist, not inherited LD_* or renderer/debug overrides.
set -- env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C \
  "HOME=$RUN/home" "TMPDIR=$RUN/tmp" "XDG_CONFIG_HOME=$RUN/config" "XDG_CACHE_HOME=$RUN/cache"
[ "${SDL_ROTATION+x}" != x ] || set -- "$@" "SDL_ROTATION=$SDL_ROTATION"
[ "${SDL_HQ_SCALER+x}" != x ] || set -- "$@" "SDL_HQ_SCALER=$SDL_HQ_SCALER"
[ "${SDL_BLITTER_DISABLED+x}" != x ] || set -- "$@" "SDL_BLITTER_DISABLED=$SDL_BLITTER_DISABLED"
[ "${SDL_ASSERT+x}" != x ] || set -- "$@" "SDL_ASSERT=$SDL_ASSERT"
[ "${SDL_GAMECONTROLLERCONFIG_FILE+x}" != x ] || set -- "$@" "SDL_GAMECONTROLLERCONFIG_FILE=$SDL_GAMECONTROLLERCONFIG_FILE"
printf 'muOS SDL display: rotation=%s scaler=%s blitter_disabled=%s\n' \
  "${SDL_ROTATION-unset}" "${SDL_HQ_SCALER-unset}" "${SDL_BLITTER_DISABLED-unset}"
# New session contains timeout + driver, not the frontend. The original launcher keeps its 90 s watchdog.
if setsid "$@" sh "$PACKAGE/run.sh" --run "$RUN/results" >"$RUN/driver.log" 2>&1; then
  DRIVER_EXIT=0
else
  DRIVER_EXIT=$?
fi
printf '%s\n' "$DRIVER_EXIT" >"$RUN/driver.exit"
if sha256sum -c "$DATA/firmware.sha256" >"$RUN/firmware-after.log" 2>&1; then
  FIRMWARE=unchanged
else
  FIRMWARE=mismatch
fi
printf 'driver_exit=%s\nfirmware=%s\nvisual_acceptance=requires_user\n' "$DRIVER_EXIT" "$FIRMWARE" >"$RUN/result.txt"
printf 'driver_exit=%s firmware=%s; exit 0 alone does not prove visible output.\n' "$DRIVER_EXIT" "$FIRMWARE"
[ "$FIRMWARE" = unchanged ] || exit 1
exit "$DRIVER_EXIT"
