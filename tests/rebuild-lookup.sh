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
mkdir -p "$conf/split" "$conf/a/dup" "$conf/b/dup" "$conf/deep/nest/lib" "$src/configs"
printf 'install:\n\t@exit 1\n' > "$src/Makefile"
for f in top.wisp lib.wisp .hidden.wisp \
         split/split.wisp split/modules.wisp \
         a/dup/dup.wisp b/dup/dup.wisp deep/nest/lib/lib.wisp; do : > "$conf/$f"; done
mkdir -p "$conf/.git"; : > "$conf/.git/skipme.wisp"
: > "$src/configs/fallback.wisp"

fail=0
run() { XDG_CONFIG_HOME=$xdg WISP_SRC=$src "$WISPCTL" rebuild "$1" 2>&1 || true; }
want() { # name expected-substring [cur = look in the remembered-config file]
    got=$(run "$1")
    if [ "$3" = cur ]; then got=$(cat "$conf/current" 2>/dev/null || echo none); fi
    case $got in *"$2"*) printf '  %-34s OK\n' "$1: $2" ;;
    *) printf '  %-34s FAIL\n    got: %s\n' "$1: $2" "$got"; fail=1 ;; esac
}

echo "==> wispctl rebuild lookup"
want top       "$conf/top.wisp"                     cur   # root file
want split     "$conf/split/split.wisp"             cur   # dir/<dir>.wisp
want split/split "$conf/split/split.wisp"           cur   # explicit dir path
want modules   "not found"                                # fragment beside it
want lib       "$conf/lib.wisp"                     cur   # root beats a nested dir
want dup       "is ambiguous, 2 matches"                  # same-depth dirs tie
want dup       "$conf/a/dup/dup.wisp"
want dup       "$conf/b/dup/dup.wisp"
want fallback  "$src/configs/fallback.wisp"         cur   # src fallback
want skipme    "searched $conf recursively"               # dot-dirs skipped
want nope      "not found"
[ $fail -eq 0 ] || { echo "rebuild-lookup: FAIL"; exit 1; }
echo "rebuild-lookup: PASS"
