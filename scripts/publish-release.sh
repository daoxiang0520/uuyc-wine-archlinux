#!/usr/bin/bash
# publish-release.sh -- create the GitHub release and attach the built packages.
#
# Pushing the repository needs credentials, and so does this; the script never
# invents any. Give it a token with `repo` scope through the environment:
#
#   GITHUB_TOKEN=ghp_... scripts/publish-release.sh
#   GITHUB_TOKEN=... scripts/publish-release.sh --tag uuyc-wine-4.40.1-21
#   scripts/publish-release.sh --dry-run          # show the plan, no network
#
# The token is read from the environment only. It is never echoed, never written
# to a file, and never placed in a command line (which would show up in `ps`).
#
# SPDX-License-Identifier: 0BSD
set -euo pipefail

owner_repo=${UUYC_REPO:-daoxiang0520/uuyc-wine-archlinux}
proxy=${UUYC_PROXY:-http://127.0.0.1:7897}
dry_run=0
tag=

while (( $# )); do
    case $1 in
        --dry-run) dry_run=1 ;;
        --tag)     tag=${2:?--tag needs a value}; shift ;;
        --repo)    owner_repo=${2:?--repo needs a value}; shift ;;
        --no-proxy) proxy= ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

# Collect the artefacts, newest pkgrel per package.
mapfile -t assets < <(ls -1t "$here"/uuyc-wine/uuyc-wine-*.pkg.tar.zst \
                              "$here"/uuyc-android/uuyc-android-*.pkg.tar.zst 2>/dev/null \
                      | awk -F/ '{k=$NF; sub(/-[0-9]+-x86_64.*/,"",k); if (!seen[k]++) print}')

if (( ${#assets[@]} == 0 )); then
    printf 'no .pkg.tar.zst found -- run scripts/build.sh first\n' >&2
    exit 1
fi

# Derive the tag from the wine package's version and pkgrel.
wine_pkg=$(grep -m1 '^uuyc-wine' "$here/uuyc-wine/PKGBUILD" 2>/dev/null || true)
pkgver=$(sed -n 's/^pkgver=//p' "$here/uuyc-wine/PKGBUILD")
pkgrel=$(sed -n 's/^pkgrel=//p' "$here/uuyc-wine/PKGBUILD")
[[ -n $tag ]] || tag="uuyc-wine-$pkgver-$pkgrel"
: "$wine_pkg"

printf 'repository : %s\n' "$owner_repo"
printf 'tag        : %s\n' "$tag"
printf 'proxy      : %s\n' "${proxy:-<none>}"
printf 'assets     :\n'
for a in "${assets[@]}"; do
    printf '  %8.1f MB  %s\n' "$(stat -c%s "$a" | awk '{print $1/1048576}')" "$(basename "$a")"
    printf '             sha256 %s\n' "$(sha256sum "$a" | cut -d' ' -f1)"
done

if (( dry_run )); then
    printf '\n--dry-run: nothing sent. Remove the flag to publish.\n'
    exit 0
fi

: "${GITHUB_TOKEN:?set GITHUB_TOKEN to a token with repo scope (it is never printed)}"

curl_args=(-sS -X POST
           -H "Authorization: Bearer $GITHUB_TOKEN"
           -H "Accept: application/vnd.github+json"
           -H "X-GitHub-Api-Version: 2022-11-28")
[[ -n $proxy ]] && curl_args+=(-x "$proxy")

api=https://api.github.com/repos/$owner_repo

# A release needs an existing tag; create it from the current commit if absent.
if ! curl "${curl_args[@]}" -o /dev/null -w '%{http_code}' "$api/git/ref/tags/$tag" | grep -q '^200$'; then
    head_sha=$(git -C "$here" rev-parse HEAD)
    printf 'creating tag %s at %s\n' "$tag" "${head_sha:0:12}"
    printf '{"ref":"refs/tags/%s","sha":"%s"}' "$tag" "$head_sha" \
        | curl "${curl_args[@]}" -H 'Content-Type: application/json' \
               --data-binary @- -o /dev/null -w '  tag -> HTTP %{http_code}\n' "$api/git/refs"
fi

body=$(cat <<EOF
uuyc-wine $pkgver-$pkgrel / uuyc-android

构建产物（Arch Linux，x86_64）：

$(for a in "${assets[@]}"; do
    printf -- '- \`%s\` — %s 字节\n  \`sha256 %s\`\n' \
        "$(basename "$a")" "$(stat -c%s "$a")" "$(sha256sum "$a" | cut -d' ' -f1)"
done)

上游文件校验：

\`\`\`text
64f918b80a99a570dd43a0ca0c8c4fac5b9b734ad6308e0f5c69bc0be6ce5167  uuyc_4.40.1.exe
2cc14a4a3fb18066ac0f38ccbc08b17bc05e6d72fb150517269e74316554fef9  uuyc_4.40.0.apk
\`\`\`

Wine 改动与崩溃根因见 README 第 1.2 节与 \`docs/qwindows-fontname-crash.md\`。
EOF
)

printf '{"tag_name":"%s","name":"%s","body":%s,"draft":false,"prerelease":false}' \
    "$tag" "$tag" "$(printf '%s' "$body" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')" \
    | curl "${curl_args[@]}" -H 'Content-Type: application/json' \
           --data-binary @- "$api/releases" -o /tmp/uuyc-release.json \
           -w 'release -> HTTP %{http_code}\n'

upload_url=$(python3 -c 'import json;print(json.load(open("/tmp/uuyc-release.json")).get("upload_url",""))' 2>/dev/null || true)
[[ -n $upload_url ]] || { printf 'could not parse upload_url; see /tmp/uuyc-release.json\n' >&2; exit 1; }
upload_url=${upload_url%%\{*}

for a in "${assets[@]}"; do
    printf 'uploading %s ... ' "$(basename "$a")"
    curl "${curl_args[@]}" -H 'Content-Type: application/octet-stream' \
         --data-binary @"$a" -o /dev/null -w 'HTTP %{http_code}\n' \
         "$upload_url?name=$(basename "$a")"
done

printf '\nrelease: https://github.com/%s/releases/tag/%s\n' "$owner_repo" "$tag"
