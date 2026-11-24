#!/usr/bin/env bash
# PortMaster / ARM64 Linux Vulkan launcher for ProjectKagemaru (Virtua Fighter)
set -euo pipefail

GAMEDIR="$(cd "$(dirname "$0")" && pwd)/projectkagemaru-vulkan"
cd "$GAMEDIR" || exit 1

exec >> "$GAMEDIR/log.txt" 2>&1
echo "=== ProjectKagemaru (Virtua Fighter ARM64 / Vulkan) ==="
echo "Date: $(date)"
echo "Arch: $(uname -m)"

./vf1-vulkan.aarch64 --data-dir "$GAMEDIR" --fullscreen "$@"
