#!/bin/sh
# wisp installer — curl-pipable. Installs wispc + wispctl and the runtime
# sources; `wispctl rebuild <config>` compiles the daemon itself on your box.
# Re-running fetches the latest version. PREFIX overrides ~/.local.
set -eu

: "${PREFIX:=$HOME/.local}"
REPO=https://github.com/darealhoks/wisp.git

fail=""
need() { command -v "$1" >/dev/null 2>&1 || fail="$fail $1"; }
need cc; need make; need git; need pkg-config
[ -z "$fail" ] || { echo "missing required tools:$fail" >&2; exit 1; }
pkg-config --exists freetype2 || {
    echo "missing: freetype2 headers (font baking needs them at build time)" >&2; exit 1; }
echo '#include <security/pam_appl.h>' | cc -E - >/dev/null 2>&1 || {
    echo "missing: PAM headers (wisp-lock-helper links libpam)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git clone -q --depth 1 "$REPO" "$tmp/wisp"
make -s -C "$tmp/wisp" install-tools install-share PREFIX="$PREFIX"

conf=${XDG_CONFIG_HOME:-$HOME/.config}/wisp
mkdir -p "$conf"

echo "installed: $PREFIX/bin/{wispc,wispctl}, sources in $PREFIX/share/wisp"
echo "your configs:    $conf/*.wisp"
echo "example configs: $PREFIX/share/wisp/configs/"
echo "docs:            $PREFIX/share/wisp/docs/"
echo "get going:       wispctl rebuild bee   (build + install + run an example)"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "note: $PREFIX/bin is not in \$PATH" ;; esac

# Optional bits: absence only disables a feature.
command -v wl-copy >/dev/null 2>&1 || echo "optional: wl-clipboard missing — emoji picker can't copy"
[ -e /usr/lib/libfreetype.so.6 ] || ldconfig -p 2>/dev/null | grep -q libfreetype.so.6 \
    || echo "optional: libfreetype.so.6 missing — the freetype font backend won't run"
