#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "ERROR: Run this script from inside the cm5audio Git repository." >&2
  exit 2
}
cd "$ROOT"
PATCH_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/connected-device-labels.patch"

if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "ERROR: The repository has tracked changes. Commit or stash them first so the patch can be safely checked." >&2
  exit 2
fi

if ! git apply --unidiff-zero --recount --check "$PATCH_FILE"; then
  echo "ERROR: Patch does not match this checkout. No files were changed." >&2
  echo "Confirm you are on the intended cm5audio main revision before trying again." >&2
  exit 1
fi

git apply --unidiff-zero --recount "$PATCH_FILE"
echo "Patch applied. Review with: git diff --check && git diff"
echo "Then build/test before committing."
