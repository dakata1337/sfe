#!/bin/sh
# Releases a new version of sfe, end to end.
#
#   tools/release.sh [OPTIONS] VERSION|major|minor|patch
#
# VERSION is X.Y.Z; major / minor / patch bump the current version (the one
# in meson.build). The changes to release are the "## Unreleased" entries of
# CHANGELOG.md, which must not be empty.
#
# What it does, stopping at the first failure:
#   1. checks: on master, no uncommitted changes to tracked files, the tag
#      does not exist yet, gh is logged in
#   2. sets the version everywhere: meson.build, docs/sfe.1 (version and
#      date), a <release> in the metainfo (its text from --summary, else the
#      changelog entries), and CHANGELOG.md (Unreleased becomes the version,
#      with a new empty Unreleased above it)
#   3. builds and tests a fresh build, validates the metainfo
#   4. commits "Release X.Y.Z", tags vX.Y.Z (annotated)
#   5. asks before publishing (unless --yes), then pushes master and the tag
#      and creates the GitHub release (source only, marked latest; the notes
#      are the changelog entries plus how to install)
#   6. sets pkgver, pkgrel=1 and the checksum of GitHub's tag archive in
#      packaging/arch/sfe/PKGBUILD, test-builds that package with makepkg
#      (unless --no-pkg-build), commits "PKGBUILD: sfe X.Y.Z" and pushes it
#
# Options:
#   -n, --dry-run        make the edits of step 2, show them, then undo them
#   -y, --yes            do not ask before publishing
#   -s, --summary TEXT   the metainfo release text (one sentence or two)
#   -i, --intro TEXT     a paragraph above the release notes
#       --no-pkg-build   skip the makepkg test build of step 6
#   -h, --help           this text
#
# Needs: git, meson, ninja, python3, gh, curl, sha256sum; appstreamcli and
# makepkg are used when installed.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

DRY_RUN=0
YES=0
SUMMARY=
INTRO=
PKG_BUILD=1
TARGET=

usage() { sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p}' "$0"; }
die() { printf 'release: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

while [ $# -gt 0 ]; do
    case $1 in
    -n | --dry-run) DRY_RUN=1 ;;
    -y | --yes) YES=1 ;;
    -s | --summary) shift; SUMMARY=${1-} ;;
    -i | --intro) shift; INTRO=${1-} ;;
    --no-pkg-build) PKG_BUILD=0 ;;
    -h | --help) usage; exit 0 ;;
    -*) die "unknown option $1 (see --help)" ;;
    *) [ -z "$TARGET" ] || die "one version only"; TARGET=$1 ;;
    esac
    shift
done
[ -n "$TARGET" ] || { usage; exit 1; }

for tool in git meson ninja python3; do
    command -v "$tool" >/dev/null || die "$tool is needed"
done

# ---------------------------------------------------------------------------
# The version

CURRENT=$(sed -n "s/^ *version: '\([0-9]*\.[0-9]*\.[0-9]*\)',$/\1/p" meson.build | head -n1)
[ -n "$CURRENT" ] || die "no version: 'X.Y.Z' line in meson.build"
case $TARGET in
major | minor | patch)
    VERSION=$(echo "$CURRENT" | awk -F. -v part="$TARGET" '{
        if (part == "major") print $1 + 1 ".0.0";
        else if (part == "minor") print $1 "." $2 + 1 ".0";
        else print $1 "." $2 "." $3 + 1 }')
    ;;
*) VERSION=$TARGET ;;
esac
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "not a version: $VERSION (X.Y.Z)"
[ "$VERSION" != "$CURRENT" ] || die "$VERSION is already the current version"
newer=$(printf '%s\n%s\n' "$CURRENT" "$VERSION" | sort -V | tail -n1)
[ "$newer" = "$VERSION" ] || die "$VERSION is older than the current $CURRENT"
TAG=v$VERSION
DATE=$(date +%Y-%m-%d)
echo "sfe $CURRENT -> $VERSION ($TAG, $DATE)"

# ---------------------------------------------------------------------------
# 1. Checks

step "Checking the repository"
BRANCH=$(git rev-parse --abbrev-ref HEAD)
[ "$BRANCH" = master ] || [ $DRY_RUN = 1 ] || die "not on master (on $BRANCH)"
git diff --quiet && git diff --cached --quiet || die "uncommitted changes to tracked files; commit or stash them"
! git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || die "tag $TAG exists already"
if [ $DRY_RUN = 0 ]; then
    for tool in gh curl sha256sum; do
        command -v "$tool" >/dev/null || die "$tool is needed"
    done
    gh auth status >/dev/null 2>&1 || die "gh is not logged in (gh auth login)"
    git fetch -q origin master
    [ "$(git rev-parse HEAD)" = "$(git rev-parse origin/master)" ] \
        || [ -z "$(git rev-list HEAD..origin/master)" ] \
        || die "origin/master has commits this checkout lacks; pull first"
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/sfe-release-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
NOTES=$WORK/notes.md

# ---------------------------------------------------------------------------
# 2. The version everywhere (and the release notes)

step "Setting the version to $VERSION"
SFE_VERSION=$VERSION SFE_DATE=$DATE SFE_SUMMARY=$SUMMARY SFE_INTRO=$INTRO SFE_NOTES=$NOTES python3 - <<'PY'
import os, re, sys
from xml.sax.saxutils import escape

version, date = os.environ["SFE_VERSION"], os.environ["SFE_DATE"]

def fail(text):
    sys.exit("release: " + text)

def edit(path, old_pattern, new, count=1, flags=0):
    text = open(path, encoding="utf-8").read()
    changed, n = re.subn(old_pattern, new, text, count=count, flags=flags)
    if n == 0:
        fail(f"{path}: nothing matched {old_pattern!r}")
    open(path, "w", encoding="utf-8").write(changed)

# CHANGELOG.md: the Unreleased entries become the version's.
text = open("CHANGELOG.md", encoding="utf-8").read()
m = re.search(r"^## Unreleased\n(.*?)(?=^## )", text, flags=re.S | re.M)
if m is None:
    fail("CHANGELOG.md has no '## Unreleased' section followed by a release")
entries = m.group(1).strip()
if not entries:
    fail("nothing under '## Unreleased' in CHANGELOG.md: write down what changed first")
text = text[: m.start()] + f"## Unreleased\n\n## {version}\n\n{entries}\n\n" + text[m.end():]
open("CHANGELOG.md", "w", encoding="utf-8").write(text)

# meson.build, the man page.
edit("meson.build", r"^(\s*version: ')\d+\.\d+\.\d+(',)$", rf"\g<1>{version}\g<2>", flags=re.M)
edit("docs/sfe.1", r'^\.TH SFE 1 \S+ "sfe [^"]*"', f'.TH SFE 1 {date} "sfe {version}"', flags=re.M)

# The metainfo: a <release> on top, its text from --summary or the entries.
def plain(markdown):
    lines = [l for l in markdown.splitlines() if not l.startswith("#")]
    bullets, current = [], []
    for line in lines:
        if line.startswith("- "):
            if current:
                bullets.append(" ".join(current))
            current = [line[2:].strip()]
        elif line.strip() and current:
            current.append(line.strip())
    if current:
        bullets.append(" ".join(current))
    text = " ".join(b if b.endswith(".") else b + "." for b in bullets)
    text = re.sub(r"[`*]", "", text)
    return re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)

summary = os.environ["SFE_SUMMARY"] or plain(entries)
release = (f'    <release version="{version}" date="{date}">\n'
           f"      <description>\n        <p>{escape(summary)}</p>\n      </description>\n"
           f"    </release>\n")
edit("data/org.sfe.SimpleFileExplorer.metainfo.xml", r"(<releases>\n)", lambda m: m.group(1) + release)

# The GitHub release notes: the entries (### headings become ##), then how
# to install.
body = re.sub(r"^### ", "## ", entries, flags=re.M)
# Relative links (docs/guide/index.md) point at the tagged files on GitHub.
body = re.sub(r"\]\((?!https?://|#)([^)]+)\)",
              rf"](https://github.com/dakata1337/sfe/blob/v{version}/\1)", body)
if not body.startswith("## "):
    body = "## What changed\n\n" + body
intro = os.environ["SFE_INTRO"]
notes = (f"{intro}\n\n" if intro else "") + body + f"""

## Installing

Download the source code below, then:

```sh
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
```

On Arch / Artix, `packaging/arch/sfe/PKGBUILD` in the repository builds this release. More in [docs/install.md](https://github.com/dakata1337/sfe/blob/v{version}/docs/install.md).
"""
open(os.environ["SFE_NOTES"], "w", encoding="utf-8").write(notes)
PY

git --no-pager diff --stat
if [ $DRY_RUN = 1 ]; then
    git --no-pager diff
    step "Release notes"
    cat "$NOTES"
    git checkout -q -- meson.build docs/sfe.1 data/org.sfe.SimpleFileExplorer.metainfo.xml CHANGELOG.md
    printf '\n(dry run: the edits are undone, nothing was committed)\n'
    exit 0
fi

# Undo the edits if a later step fails before the release commit.
COMMITTED=0
cleanup() {
    [ $COMMITTED = 1 ] || git checkout -q -- meson.build docs/sfe.1 data/org.sfe.SimpleFileExplorer.metainfo.xml CHANGELOG.md
    rm -rf "$WORK"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 3. Build and test

step "Building and testing a fresh build"
meson setup "$WORK/build" >"$WORK/setup.log" 2>&1 || { cat "$WORK/setup.log"; die "meson setup failed"; }
ninja -C "$WORK/build" >"$WORK/build.log" 2>&1 || { tail -n 30 "$WORK/build.log"; die "the build failed"; }
meson test -C "$WORK/build" --print-errorlogs >"$WORK/test.log" 2>&1 || { tail -n 40 "$WORK/test.log"; die "tests failed"; }
grep -E '^(Ok|Fail):' "$WORK/test.log"
built=$("$WORK/build/sfe" --version)
[ "$built" = "sfe $VERSION" ] || die "the build says '$built'"
if command -v appstreamcli >/dev/null; then
    appstreamcli validate --no-net data/org.sfe.SimpleFileExplorer.metainfo.xml >/dev/null \
        || die "the metainfo does not validate (appstreamcli validate --no-net data/*.metainfo.xml)"
fi

# ---------------------------------------------------------------------------
# 4. Commit and tag

step "Committing and tagging $TAG"
git add meson.build docs/sfe.1 data/org.sfe.SimpleFileExplorer.metainfo.xml CHANGELOG.md
git commit -q -m "Release $VERSION"
COMMITTED=1
git tag -a "$TAG" -m "sfe $VERSION"

# ---------------------------------------------------------------------------
# 5. Publish

if [ $YES = 0 ]; then
    printf '\nPush master and %s, and publish the GitHub release? [y/N] ' "$TAG"
    read -r answer
    case $answer in
    y | Y | yes) ;;
    *)
        echo "Not published. The release commit and tag are local; to drop them:"
        echo "  git tag -d $TAG && git reset --hard HEAD~1"
        exit 1
        ;;
    esac
fi
step "Publishing"
git push -q origin master
git push -q origin "$TAG"
gh release create "$TAG" --verify-tag --latest --title "sfe $VERSION" --notes-file "$NOTES"

# ---------------------------------------------------------------------------
# 6. The release PKGBUILD

step "Updating packaging/arch/sfe/PKGBUILD"
PKGBUILD=packaging/arch/sfe/PKGBUILD
ARCHIVE=$WORK/$TAG.tar.gz
URL=https://github.com/dakata1337/sfe/archive/refs/tags/$TAG.tar.gz
tries=0
until curl -sfL -o "$ARCHIVE" "$URL"; do # the archive can lag behind the tag
    tries=$((tries + 1))
    [ $tries -lt 10 ] || die "cannot download $URL"
    sleep 3
done
SUM=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
sed -i "s/^pkgver=.*/pkgver=$VERSION/; s/^pkgrel=.*/pkgrel=1/; s/^sha256sums=('[0-9a-f]*')/sha256sums=('$SUM')/" "$PKGBUILD"
grep -q "^sha256sums=('$SUM')" "$PKGBUILD" || die "could not set the checksum in $PKGBUILD"
echo "sha256 $SUM"

if [ $PKG_BUILD = 1 ] && command -v makepkg >/dev/null; then
    step "Test-building the package"
    mkdir "$WORK/pkg"
    cp "$PKGBUILD" "$WORK/pkg/"
    (cd "$WORK/pkg" && makepkg -f --nodeps >"$WORK/makepkg.log" 2>&1) \
        || { tail -n 30 "$WORK/makepkg.log"; die "makepkg failed; $PKGBUILD is changed but not committed"; }
    grep -E '^(Ok|Fail):' "$WORK/makepkg.log" | tail -n 2 || true
fi

git add "$PKGBUILD"
git commit -q -m "PKGBUILD: sfe $VERSION"
git push -q origin master

step "Released sfe $VERSION"
gh release view "$TAG" --json url --jq .url
