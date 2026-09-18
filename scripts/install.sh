#!/usr/bin/bash
# Install the built packages and finish the host-side setup.
#
#   ./scripts/install.sh              install both packages and prepare Waydroid
#   ./scripts/install.sh --wine-only  install only the Wine client
#   ./scripts/install.sh --apk-only   install only the Android client
#   ./scripts/install.sh --no-setup   skip Waydroid initialisation
#
# Everything below `pacman -U` runs as the invoking user; the only privileged
# steps are the package install itself and Waydroid initialisation.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
readonly root

wine_only=false
apk_only=false
do_setup=true
for arg in "$@"; do
    case $arg in
        --wine-only) wine_only=true ;;
        --apk-only) apk_only=true ;;
        --no-setup) do_setup=false ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) printf 'unknown option: %s\n' "$arg" >&2; exit 2 ;;
    esac
done

mapfile -t packages < <(find "$root" -maxdepth 2 -name '*.pkg.tar.zst' | sort)
((${#packages[@]} > 0)) || { printf 'no packages found, run scripts/build.sh first\n' >&2; exit 1; }

selected=()
for pkg in "${packages[@]}"; do
    case $pkg in
        *uuyc-wine*) $apk_only || selected+=("$pkg") ;;
        *uuyc-android*) $wine_only || selected+=("$pkg") ;;
    esac
done

printf '=== installing ===\n'
printf '  %s\n' "${selected[@]}"
sudo pacman -U --needed "${selected[@]}"

if $wine_only; then
    printf '\nLaunch the client with: uuyc-wine\n'
    exit 0
fi

printf '\n=== checking the Android container prerequisites ===\n'
uuyc-android --status || true

if $do_setup && [[ $(uname -m) == x86_64 ]]; then
    cat <<'EOF'

The Android client ships arm64-v8a native code only, so an x86_64 host needs
Waydroid's ARM translation layer. Finish the setup with:

  sudo modprobe binder_linux            # modules: binder_linux, ashmem_linux
  sudo waydroid init -s GAPPS           # downloads the Android image (~1 GB)
  sudo uuyc-android-arm                 # installs libndk and enables it
  waydroid session start                # keep this running
  uuyc-android                          # installs the APK, then launches it

On an aarch64 host only the waydroid init and session steps are needed.
EOF
fi

if ! $apk_only; then
    printf '\nLaunch the Wine client with: uuyc-wine\n'
fi
printf 'Verify the adaptation at any time with: uuyc-android-verify\n'
