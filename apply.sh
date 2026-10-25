#!/usr/bin/env bash
set -euo pipefail

PATCH_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PATCH_FILE="$PATCH_DIR/fix.patch"

files=(
  include/device_manager.h
  include/dsp_engine.h
  src/device_manager.cpp
  src/dsp_engine.cpp
  src/main.cpp
  web_client/dashboard.html
)

for file in "${files[@]}"; do
  if [[ ! -f "$file" ]]; then
    echo "ERROR: run this script from the cm5audio project root; missing $file" >&2
    exit 1
  fi
done

if [[ ! -f "$PATCH_FILE" ]]; then
  echo "ERROR: missing patch file: $PATCH_FILE" >&2
  exit 1
fi

# Dry-run must succeed completely before any file is backed up or modified.
echo "Checking patch applicability (dry run; no files modified)..."
if ! patch --dry-run --batch -p1 < "$PATCH_FILE"; then
  echo >&2
  echo "Patch did not match. No backups were created and no source files were modified." >&2
  exit 1
fi

backup_suffix=".before-pipewire-vu-v2-$(date +%Y%m%d-%H%M%S)"
for file in "${files[@]}"; do
  cp -a -- "$file" "${file}${backup_suffix}"
done

echo "Applying corrected PipeWire VU-meter patch..."
patch --batch -p1 < "$PATCH_FILE"
echo
echo "Patch applied. Backups use suffix: $backup_suffix"
echo "Next: rebuild using your normal CM5Audio build process and inspect /api/meters."
