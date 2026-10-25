#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
PATCH_FILE="$SCRIPT_DIR/fix.patch"
cd "$ROOT_DIR"

for file in include/device_manager.h src/device_manager.cpp web_client/dashboard.html; do
    if [[ ! -f "$file" ]]; then
        echo "Required file not found: $file" >&2
        exit 1
    fi
done

echo "Checking PipeWire live-graph refresh patch v5 (dry run; no files modified)..."
patch --dry-run --forward --batch -p1 < "$PATCH_FILE"

stamp="$(date +%Y%m%d-%H%M%S)"
for file in include/device_manager.h src/device_manager.cpp web_client/dashboard.html; do
    cp -a -- "$file" "$file.before-pw-live-v5-$stamp"
done

echo "Applying PipeWire live-graph refresh patch..."
patch --forward --batch -p1 < "$PATCH_FILE"
echo "Patch applied. Backups use suffix: .before-pw-live-v5-$stamp"
echo "Next: make -j7"
