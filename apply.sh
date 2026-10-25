#!/usr/bin/env bash
set -euo pipefail

PATCH_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PATCH_FILE="$PATCH_DIR/fix.patch"

for file in include/device_manager.h src/device_manager.cpp; do
  if [[ ! -f "$file" ]]; then
    echo "ERROR: run this script from the cm5audio project root; missing $file" >&2
    exit 1
  fi
done
if [[ ! -f "$PATCH_FILE" ]]; then
  echo "ERROR: missing patch file: $PATCH_FILE" >&2
  exit 1
fi

# Do not create backups or modify files unless the entire patch dry-runs cleanly.
echo "Checking PipeWire self-monitor filtering patch v4 (dry run; no files modified)..."
if ! patch --dry-run --batch -p1 < "$PATCH_FILE"; then
  echo >&2
  echo "Patch did not match. No backups were created and no source files were modified." >&2
  exit 1
fi

suffix=".before-pw-filter-v4-$(date +%Y%m%d-%H%M%S)"
cp -a include/device_manager.h "include/device_manager.h${suffix}"
cp -a src/device_manager.cpp "src/device_manager.cpp${suffix}"

echo "Applying self-monitor node/link filtering..."
patch --batch -p1 < "$PATCH_FILE"
echo "Patch applied. Backups use suffix: ${suffix}"
echo "Rebuild with: make -j7"
