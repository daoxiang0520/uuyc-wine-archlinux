#!/usr/bin/bash
# Convenience wrapper: build both Arch Linux packages from this directory.
set -euo pipefail
exec "$(dirname -- "${BASH_SOURCE[0]}")/scripts/build.sh" "$@"
