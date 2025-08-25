#!/usr/bin/env bash
set -euo pipefail
if [[ ! -d build ]]; then
  meson setup --buildtype=release build
else
  meson setup --reconfigure --buildtype=release build
fi
meson compile -C build
