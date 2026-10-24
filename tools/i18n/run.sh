#!/bin/sh
# UI translations (see po/README.md), run from the repository root.
#   tools/i18n/run.sh update WORKDIR   re-extract po/mellowtone.pot, merge it into every
#                                        po/*.po, regenerate src/ui/i18n_catalog.c
#   tools/i18n/run.sh check  WORKDIR   the same checks, writing nothing in the tree
# Used by the CMake targets `i18n` / `i18n-check` (./mt i18n [check]).
set -eu
mode=${1:?usage: run.sh update|check WORKDIR}
work=${2:?usage: run.sh update|check WORKDIR}
mkdir -p "$work"

find src -type f \( -name '*.c' -o -name '*.h' \) \
    ! -name '*_test.c' ! -name '*_check.c' ! -name '*_regression.c' ! -name i18n_catalog.c \
    | LC_ALL=C sort > "$work/sources.txt"
xgettext --language=C --from-code=UTF-8 --no-location \
    --keyword=TR --keyword=N_ --keyword=TR_N:1,2 --keyword=TR_C:1c,2 \
    --add-comments=TRANSLATORS --package-name=mellowtone --msgid-bugs-address=none \
    -o "$work/mellowtone.pot.new" -f "$work/sources.txt"

case "$mode" in
update)
    if [ ! -f po/mellowtone.pot ] || ! diff -q -I '^"POT-Creation-Date' "$work/mellowtone.pot.new" po/mellowtone.pot >/dev/null; then
        cp "$work/mellowtone.pot.new" po/mellowtone.pot
        echo "po/mellowtone.pot updated"
    fi
    for po in po/*.po; do
        msgmerge --update --backup=none --no-wrap -q "$po" po/mellowtone.pot
        msgfmt -c --check-format -o /dev/null "$po"
    done
    python3 tools/i18n/po2c.py
    ;;
check)
    diff -q -I '^"POT-Creation-Date' "$work/mellowtone.pot.new" po/mellowtone.pot >/dev/null \
        || { echo "po/mellowtone.pot is stale; run ./mt i18n"; exit 1; }
    for po in po/*.po; do
        msgfmt -c --check-format --statistics -o /dev/null "$po"
    done
    python3 tools/i18n/po2c.py --check
    python3 tools/i18n/find_untranslated.py
    ;;
*) echo "run.sh: unknown mode '$mode'" >&2; exit 2 ;;
esac
