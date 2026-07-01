#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -d "$DIR/assets" ]; then
  ASSETS="$DIR/assets"
else
  ASSETS="$DIR/../../../assets"
fi

set -- --game_data_root "$ASSETS" --gpu_plugin=xenos --license_mask=1 "$@"

if [ -d "$DIR/update" ]; then
  set -- --update_data_root "$DIR/update" "$@"
elif [ -d "$DIR/../../../update" ]; then
  set -- --update_data_root "$DIR/../../../update" "$@"
fi

if [ -d "$DIR/mods" ]; then
  set -- --mods_data_root "$DIR/mods" "$@"
elif [ -d "$DIR/../../../mods" ]; then
  set -- --mods_data_root "$DIR/../../../mods" "$@"
fi

exec "$DIR/{{EXE}}" "$@"
