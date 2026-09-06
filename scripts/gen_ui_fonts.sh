#!/usr/bin/env bash
# Regenerate the vendored JetBrains Mono LVGL font tables.
#
# The UI's numeric readouts are set in a monospaced face so tabular values stop
# jittering in width as digits change (docs/ui-design-constraints.md,
# "Typography"). LVGL ships no monospaced font, so the tables are generated
# once and committed - the build must not depend on node or a system font.
# Re-run this only when the ladder in ui_theme.h changes.
#
# The output directory is excluded from clang-format and the whitespace
# hooks in .pre-commit-config.yaml: reformatting it would make the committed
# artifact differ from what this script produces, so every regeneration
# would show up as a 20,000-line diff.
#
# Sizes and source weights mirror the type ladder: small readouts in Medium
# (500), hero values in SemiBold (600). Subset is printable ASCII only; a glyph
# outside 0x20-0x7E renders as a box, so widen the range here rather than
# working around it at the call site.
set -euo pipefail

OUT="$(cd "$(dirname "$0")/.." && pwd)/firmware/esp32/components/ui/fonts"
FONTDIR="${WAVEX_JBM_DIR:-$HOME/.local/share/fonts/nerd-fonts/JetBrainsMono}"
CONV="${LV_FONT_CONV:-npx --yes lv_font_conv@1.5.3}"

gen() {  # gen <size> <weight-file> <bpp>
    local size="$1" face="$2"
    local src="$FONTDIR/JetBrainsMonoNerdFont-$face.ttf"
    [ -f "$src" ] || { echo "missing $src" >&2; exit 1; }
    echo "  wavex_mono_$size  ($face)"
    $CONV --font "$src" -r '0x20-0x7E' --size "$size" --bpp 4 --no-compress \
          --force-fast-kern-format --format lvgl --lv-include lvgl.h \
          --lv-font-name "wavex_mono_$size" -o "$OUT/wavex_mono_$size.c"
    # Strip absolute paths out of the banner so the committed artifact is
    # identical whoever regenerates it.
    sed -i -e "s#$src#JetBrainsMonoNerdFont-$face.ttf#g" \
           -e "s#$OUT/#firmware/esp32/components/ui/fonts/#g" \
           "$OUT/wavex_mono_$size.c"
}

echo "Generating LVGL mono fonts into $OUT"
gen 14 Medium
gen 18 Medium
gen 26 SemiBold
gen 38 SemiBold
echo "Done. Rebuild the ESP32 target to pick them up."
