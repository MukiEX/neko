#!/bin/sh
# Starts an app only if it is listed in NEKO_SURROUND_APPS (comma or space
# separated, default "firefox,vlc"). A disabled app idles instead of exiting,
# so supervisord does not keep restarting it.
#
#   run-app.sh <name> <command> [args...]

name="$1"
shift

apps=$(printf '%s' "${NEKO_SURROUND_APPS:-firefox,vlc}" | tr ',' ' ')

for app in $apps; do
  if [ "$app" = "$name" ]; then
    exec "$@"
  fi
done

echo "$name is not in NEKO_SURROUND_APPS ($apps), not starting it"
exec sleep infinity
