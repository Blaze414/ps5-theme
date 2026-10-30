#!/usr/bin/env bash
# Rebuild a release from its tag, from nothing, and compare the result with the files published with it, byte for
# byte. Needs Docker, git and network access (the release's files are downloaded from GitHub).
#
#   tools/verify-release.sh v1.80
#
# Prints each file's SHA-256, built and published, and exits non-zero if any differ. See
# docs/building.md#reproducible-builds.
set -euo pipefail
tag=${1:?usage: tools/verify-release.sh <tag, such as v1.80>}
repo=${PS5THEME_REPO:-Blaze414/ps5-theme}
cd "$(dirname "$0")/.."

work=$(mktemp -d)
cleanup() { git worktree remove --force "$work/src" >/dev/null 2>&1 || true; rm -rf "$work"; }
trap cleanup EXIT

git fetch -q --tags origin 2>/dev/null || true
git worktree add -q --detach "$work/src" "$tag"
echo "Building $tag from nothing (this takes a few minutes)..."
docker build --platform linux/amd64 --no-cache --target artifact --output "type=local,dest=$work/built" "$work/src" > "$work/build.log" 2>&1 \
  || { tail -20 "$work/build.log"; echo "the build failed" >&2; exit 2; }

sha() { python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
status=0
for f in ps5-theme.plugin ps5-theme.elf ps5-theme-stop.elf; do
  curl -fsSL -o "$work/$f" "https://github.com/$repo/releases/download/$tag/$f"
  built=$(sha "$work/built/$f") published=$(sha "$work/$f")
  if [ "$built" = "$published" ]; then
    echo "same      $built  $f"
  else
    echo "DIFFERENT $f: built $built, published $published"
    status=1
  fi
done
[ $status = 0 ] && echo "$tag: the published files are exactly what its source builds." || echo "$tag: NOT the same." >&2
exit $status
