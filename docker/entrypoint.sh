#!/bin/sh
# Kenshi-Online server container entrypoint.
#
# Runs as root, fixes /data ownership for the runtime user, then drops
# privileges via gosu and exec's the server. Honors PUID/PGID env vars
# so Unraid users can match the host's nobody:users (99:100) and keep
# appdata ownership tidy.
set -e

PUID="${PUID:-10001}"
PGID="${PGID:-10001}"

mkdir -p /data
# chown -R is cheap on a fresh dir; on a populated one it's still O(files)
# but the server's save tree stays small (entities + JSON).
chown -R "$PUID:$PGID" /data

# tini reaps zombies and forwards SIGTERM so the server's atexit save runs
# cleanly on `docker stop` / Unraid Stop.
exec tini -- gosu "$PUID:$PGID" /usr/local/bin/KenshiMP.Server "$@"
