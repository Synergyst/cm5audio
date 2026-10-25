#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$PWD" rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"
echo "Applying PipeWire node-info multichannel fix v8.1 (all source anchors are checked before files are changed)..."
python3 "$SCRIPT_DIR/apply_v8.py"
