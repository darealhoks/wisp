#!/bin/sh
# wisp installer — curl-pipable. Installs wispc + wispctl and the runtime
# sources; `wispctl rebuild <config>` compiles the daemon itself on your box.
# Re-running fetches the latest version. PREFIX overrides ~/.local.
set -eu

: "${PREFIX:=$HOME/.local}"
REPO=https://github.com/darealhoks/wisp.git

if [ -t 1 ]; then
    B=$(printf '\033[1m'); G=$(printf '\033[32m'); Y=$(printf '\033[33m')
    R=$(printf '\033[31m'); N=$(printf '\033[0m')
else
    B=; G=; Y=; R=; N=
fi
step() { printf '%s::%s %s\n' "$G$B" "$N$B" "$1$N"; }
err()  { printf '%serror:%s %s\n' "$R$B" "$N" "$1" >&2; exit 1; }

step "checking dependencies"
fail=""
need() { command -v "$1" >/dev/null 2>&1 || fail="$fail $1"; }
need cc; need make; need git; need pkg-config
[ -z "$fail" ] || err "missing required tools:$fail"
pkg-config --exists freetype2 || err "freetype2 headers missing (font baking needs them at build time)"
echo '#include <security/pam_appl.h>' | cc -E - >/dev/null 2>&1 \
    || err "PAM headers missing (wisp-lock-helper links libpam)"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
step "fetching $REPO"
git clone --depth 1 --progress "$REPO" "$tmp/wisp"
step "installing tools + sources to $PREFIX"
make -s -C "$tmp/wisp" install-tools install-share PREFIX="$PREFIX"

conf=${XDG_CONFIG_HOME:-$HOME/.config}/wisp
mkdir -p "$conf"

step "done"
echo "  installed:       $PREFIX/bin/{wispc,wispctl}, sources in $PREFIX/share/wisp"
echo "  your configs:    $conf/*.wisp"
echo "  example configs: $PREFIX/share/wisp/configs/"
echo "  docs:            $PREFIX/share/wisp/docs/"
echo "  get going:       ${B}wispctl rebuild bee$N   (build + install + run an example)"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "${Y}note:$N $PREFIX/bin is not in \$PATH" ;; esac

# Optional bits: absence only disables a feature.
command -v wl-copy >/dev/null 2>&1 || echo "${Y}optional:$N wl-clipboard missing — emoji picker can't copy"
[ -e /usr/lib/libfreetype.so.6 ] || ldconfig -p 2>/dev/null | grep -q libfreetype.so.6 \
    || echo "${Y}optional:$N libfreetype.so.6 missing — the freetype font backend won't run"
