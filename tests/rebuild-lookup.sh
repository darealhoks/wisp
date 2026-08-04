#!/bin/sh
# `wispctl rebuild <name>` config discovery. Runs against a throwaway
# XDG_CONFIG_HOME and a $WISP_SRC whose `install` target always fails, so the
# check stops right after resolution — it never builds and never touches a
# running daemon.
set -e
WISPCTL=${1:?usage: rebuild-lookup.sh <path-to-wispctl>}
WISPCTL=$(realpath "$WISPCTL")
tmp=$(realpath "$(mktemp -d)"); trap 'rm -rf "$tmp"' EXIT
xdg=$tmp/xdg; conf=$xdg/wisp; src=$tmp/src   # wispctl appends /wisp to XDG_CONFIG_HOME
mkdir -p "$conf/config1/config1-1" "$conf/dup-a" "$conf/dup-b" "$conf/deep" "$src/configs"
printf 'install:\n\t@exit 1\n' > "$src/Makefile"
for f in config1/config1-1/config.wisp dup-a/dup.wisp dup-b/dup.wisp \
         top.wisp deep/top.wisp .hidden.wisp; do : > "$conf/$f"; done
mkdir -p "$conf/.git"; : > "$conf/.git/skipme.wisp"
: > "$src/configs/fallback.wisp"

fail=0
run() { XDG_CONFIG_HOME=$xdg WISP_SRC=$src "$WISPCTL" rebuild "$1" 2>&1 || true; }
want() { # name expected-substring [in current-file | in output]
    got=$(run "$1")
    if [ "$3" = cur ]; then got=$(cat "$conf/current" 2>/dev/null || echo none); fi
    case $got in *"$2"*) printf '  %-34s OK\n' "$1: $2" ;;
    *) printf '  %-34s FAIL\n    got: %s\n' "$1: $2" "$got"; fail=1 ;; esac
}

echo "==> wispctl rebuild lookup"
want config   "$conf/config1/config1-1/config.wisp" cur   # nested, recursive
want top      "$conf/top.wisp"                      cur   # shallowest wins
want dup      "is ambiguous, 2 matches"                   # tie is an error
want dup      "$conf/dup-a/dup.wisp"                      # ...listing each one
want dup      "$conf/dup-b/dup.wisp"
want fallback "$src/configs/fallback.wisp"          cur   # src fallback
want skipme   "searched $conf recursively"                # dot-dirs skipped
want nope     "not found"
[ $fail -eq 0 ] || { echo "rebuild-lookup: FAIL"; exit 1; }
echo "rebuild-lookup: PASS"
