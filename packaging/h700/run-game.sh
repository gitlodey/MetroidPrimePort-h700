#!/bin/sh
# Installed inside the physical .metroid-prime-unbound-h700 payload.
set -eu
umask 077
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

fail() {
  printf 'H700 launcher: %s; nothing launched\n' "$1" >&2
  exit 2
}

[ "$#" -eq 1 ] || fail 'expected exactly one absolute readable disc path'
DISC=$1
case "$DISC" in
  /*) ;;
  *) fail 'disc path must be absolute (configure DISC in the Ports entry)' ;;
esac
[ -f "$DISC" ] && [ -r "$DISC" ] || fail 'disc must be a readable regular file'
DATA=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || fail 'cannot resolve payload directory'
GAME="$DATA/metroid_prime_port"
[ ! -L "$GAME" ] && [ -f "$GAME" ] && [ -x "$GAME" ] || fail 'metroid_prime_port must be a real regular executable, not a symlink'

# Keep this policy identical to PortH700::ConflictingOverride, before env -i.
# Only literal variable NAMES expand here, never caller values or shell code.
for conflict in \
  ${MP_FRAME_BUFFERS+MP_FRAME_BUFFERS} \
  ${MP_STORAGE_BUFFERS+MP_STORAGE_BUFFERS} \
  ${AURORA_FULL_DEVICE_BUFFERS+AURORA_FULL_DEVICE_BUFFERS} \
  ${AURORA_STAGING_START_KIB+AURORA_STAGING_START_KIB} \
  ${AURORA_STAGING_START_MASK+AURORA_STAGING_START_MASK} \
  ${MP_DAWN_ENABLE:+MP_DAWN_ENABLE} \
  ${MP_DAWN_DISABLE:+MP_DAWN_DISABLE} \
  ${MP_GRAPHICS_BACKEND:+MP_GRAPHICS_BACKEND} \
  ${AURORA_PRESENT_EVERY:+AURORA_PRESENT_EVERY} \
  ${MP_PIPELINE_THREADS:+MP_PIPELINE_THREADS} \
  ${MP_GL_VERSION:+MP_GL_VERSION} \
  ${MP_GPU_POWER:+MP_GPU_POWER} \
  ${AURORA_VERTEX_FETCH_TEXTURE:+AURORA_VERTEX_FETCH_TEXTURE} \
  ${AURORA_NO_COMPUTE:+AURORA_NO_COMPUTE} \
  ${MP_GPU_NO_CORE:+MP_GPU_NO_CORE} \
  ${AURORA_NO_BC:+AURORA_NO_BC} \
  ${MP_RESIZE_WAIT:+MP_RESIZE_WAIT} \
  ${MP_WGSL_OVERRIDE:+MP_WGSL_OVERRIDE} \
  ${MP_WGSL_DUMP:+MP_WGSL_DUMP} \
  ${AURORA_GPU_SELFTEST:+AURORA_GPU_SELFTEST} \
  ${MP_GPU_SELFTEST:+MP_GPU_SELFTEST} \
  ${AURORA_OVERDRAW:+AURORA_OVERDRAW} \
  ${MP_STORAGE_CLAMP:+MP_STORAGE_CLAMP} \
  ${MP_MEM1_MB:+MP_MEM1_MB} \
  ${MP_ROOM_GEO_RESIDENT:+MP_ROOM_GEO_RESIDENT} \
  ${MP_ROOM_GEO_RESIDENT_MB:+MP_ROOM_GEO_RESIDENT_MB} \
  ${MP_PIPELINE_LAZY:+MP_PIPELINE_LAZY} \
  ${MP_PIPELINE_SEED:+MP_PIPELINE_SEED} \
  ${MP_PIPELINE_PBR:+MP_PIPELINE_PBR} \
  ${MP_GPU_DRIVER:+MP_GPU_DRIVER} \
  ${MP_PIPELINE_TRIM_MIB:+MP_PIPELINE_TRIM_MIB} \
  ${MP_PIPELINE_TRIM_SEVERE_MIB:+MP_PIPELINE_TRIM_SEVERE_MIB} \
  ${MP_PIPELINE_EVICT:+MP_PIPELINE_EVICT} \
  ${MP_TEXTURE_PRESSURE_MIB:+MP_TEXTURE_PRESSURE_MIB} \
  ${MP_TEXTURE_PRESSURE_SEVERE_MIB:+MP_TEXTURE_PRESSURE_SEVERE_MIB}; do
  fail "conflicting H700 profile variable: $conflict"
done
[ "${SDL_VIDEODRIVER-sdl2}" = sdl2 ] || fail 'conflicting H700 profile variable: SDL_VIDEODRIVER'
[ "${SDL3SHIM_SDL2_VIDEODRIVER-mali}" = mali ] || fail 'conflicting H700 profile variable: SDL3SHIM_SDL2_VIDEODRIVER'
[ "${MP_OPENGLES-1}" = 1 ] || fail 'conflicting H700 profile variable: MP_OPENGLES'

for directory in "$DATA/user" "$DATA/cache" "$DATA/user/tmp"; do
  [ ! -L "$directory" ] || fail 'user/cache/tmp must not be symlinks into another installation'
  mkdir -p -- "$directory" || fail 'cannot create own user/cache/tmp'
  [ -d "$directory" ] && [ -w "$directory" ] && [ -x "$directory" ] || fail 'own user/cache/tmp must be writable directories'
done
cd -- "$DATA" || fail 'cannot enter payload directory'
ulimit -c 0

set -- env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C \
  "HOME=$DATA/user" "TMPDIR=$DATA/user/tmp" \
  "XDG_CONFIG_HOME=$DATA/user" "XDG_CACHE_HOME=$DATA/cache" \
  "MP_USER_PATH=$DATA/user" "MP_CACHE_PATH=$DATA/cache" MP_LOG_FILE=1 \
  SDL_VIDEODRIVER=sdl2 SDL3SHIM_SDL2_VIDEODRIVER=mali MP_OPENGLES=1
# Explicit diagnostics only; renderer/profile overrides remain rejected above.
[ "${MP_H700_GL_TIMING+x}" != x ] || set -- "$@" "MP_H700_GL_TIMING=$MP_H700_GL_TIMING"
[ "${MP_H700_BENCH+x}" != x ] || set -- "$@" "MP_H700_BENCH=$MP_H700_BENCH"
case "${MP_H700_BENCH-}" in
  1|menu)
    [ "${MP_FAST_BOOT+x}" != x ] || set -- "$@" "MP_FAST_BOOT=$MP_FAST_BOOT"
    [ "${MP_H700_DRAW_UBO+x}" != x ] || set -- "$@" "MP_H700_DRAW_UBO=$MP_H700_DRAW_UBO"
    [ "${MP_H700_FETCH_GATHER+x}" != x ] || set -- "$@" "MP_H700_FETCH_GATHER=$MP_H700_FETCH_GATHER"
    [ "${MP_H700_NATIVE_VERTEX+x}" != x ] || set -- "$@" "MP_H700_NATIVE_VERTEX=$MP_H700_NATIVE_VERTEX"
    [ "${MP_H700_PROGRAM_CACHE+x}" != x ] || set -- "$@" "MP_H700_PROGRAM_CACHE=$MP_H700_PROGRAM_CACHE"
    [ "${MP_H700_PRODUCER_FLUSH+x}" != x ] || set -- "$@" "MP_H700_PRODUCER_FLUSH=$MP_H700_PRODUCER_FLUSH"
    [ "${MP_H700_MOVIE_CLIENT_UPLOAD+x}" != x ] || set -- "$@" "MP_H700_MOVIE_CLIENT_UPLOAD=$MP_H700_MOVIE_CLIENT_UPLOAD"
    [ "${MP_H700_FRAME_DEST_RING+x}" != x ] || set -- "$@" "MP_H700_FRAME_DEST_RING=$MP_H700_FRAME_DEST_RING"
    [ "${MP_H700_RETAIL_GEOMETRY+x}" != x ] || set -- "$@" "MP_H700_RETAIL_GEOMETRY=$MP_H700_RETAIL_GEOMETRY"
    ;;
esac
[ "${MP_PASS_LOG+x}" != x ] || set -- "$@" "MP_PASS_LOG=$MP_PASS_LOG"
[ "${AURORA_FRAME_PEAKS+x}" != x ] || set -- "$@" "AURORA_FRAME_PEAKS=$AURORA_FRAME_PEAKS"
[ "${SDL_ROTATION+x}" != x ] || set -- "$@" "SDL_ROTATION=$SDL_ROTATION"
[ "${SDL_HQ_SCALER+x}" != x ] || set -- "$@" "SDL_HQ_SCALER=$SDL_HQ_SCALER"
[ "${SDL_BLITTER_DISABLED+x}" != x ] || set -- "$@" "SDL_BLITTER_DISABLED=$SDL_BLITTER_DISABLED"
[ "${SDL_ASSERT+x}" != x ] || set -- "$@" "SDL_ASSERT=$SDL_ASSERT"
[ "${SDL_GAMECONTROLLERCONFIG_FILE+x}" != x ] || set -- "$@" "SDL_GAMECONTROLLERCONFIG_FILE=$SDL_GAMECONTROLLERCONFIG_FILE"
[ "${SDL_AUDIODRIVER+x}" != x ] || set -- "$@" "SDL_AUDIODRIVER=$SDL_AUDIODRIVER"
[ "${SDL_AUDIO_DRIVER+x}" != x ] || set -- "$@" "SDL_AUDIO_DRIVER=$SDL_AUDIO_DRIVER"
[ "${ALSA_CONFIG_PATH+x}" != x ] || set -- "$@" "ALSA_CONFIG_PATH=$ALSA_CONFIG_PATH"
[ "${ALSA_CONFIG_DIR+x}" != x ] || set -- "$@" "ALSA_CONFIG_DIR=$ALSA_CONFIG_DIR"
[ "${ALSA_CARD+x}" != x ] || set -- "$@" "ALSA_CARD=$ALSA_CARD"
[ "${ALSA_PCM_CARD+x}" != x ] || set -- "$@" "ALSA_PCM_CARD=$ALSA_PCM_CARD"
[ "${PULSE_SERVER+x}" != x ] || set -- "$@" "PULSE_SERVER=$PULSE_SERVER"
[ "${PULSE_RUNTIME_PATH+x}" != x ] || set -- "$@" "PULSE_RUNTIME_PATH=$PULSE_RUNTIME_PATH"
[ "${PIPEWIRE_REMOTE+x}" != x ] || set -- "$@" "PIPEWIRE_REMOTE=$PIPEWIRE_REMOTE"
[ "${XDG_RUNTIME_DIR+x}" != x ] || set -- "$@" "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"
if [ "${PIPEWIRE_RUNTIME_DIR+x}" = x ]; then
  set -- "$@" "PIPEWIRE_RUNTIME_DIR=$PIPEWIRE_RUNTIME_DIR"
elif [ -S /run/pipewire-0 ]; then
  set -- "$@" PIPEWIRE_RUNTIME_DIR=/run
fi

mkdir -- "$DATA/run.lock" 2>/dev/null || fail 'already running or lock retained; do not remove it while child state is unknown'
# POSIX sh defers these traps until the FOREGROUND command has completed/reaped.
# Do not use an interruptible background wait or EXIT cleanup: either can unlock
# a still-live/unknown game. env execs the actual game, not its log copier.
trap ':' HUP INT TERM
if "$@" "$GAME" "$DISC" >/dev/null 2>&1; then
  status=0
else
  status=$?
fi
# Only this known child completion permits unlocking; never wait for descendants.
rmdir -- "$DATA/run.lock" || { printf '%s\n' 'H700 launcher: could not release completed game lock' >&2; }
exit "$status"
