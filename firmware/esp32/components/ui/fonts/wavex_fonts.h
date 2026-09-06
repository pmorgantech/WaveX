/**
 * @file wavex_fonts.h
 * @brief Vendored monospaced font tables for numeric readouts.
 *
 * LVGL ships no monospaced face, so these four tables are generated from
 * JetBrains Mono by `scripts/gen_ui_fonts.sh` and committed - the build must
 * not depend on node or on a font being installed on the developer's machine.
 *
 * They exist so that tabular values (times, Hz, dB, percentages, counters)
 * stop changing width as their digits change. Use them for values that are
 * read as numbers; Montserrat stays the face for everything that is read as
 * prose. The size ladder is defined in `styles/ui_theme.h` - prefer the
 * `UI_FONT_*` role macros there over naming a table directly, so a ladder
 * change lands in one file.
 *
 * Subset is printable ASCII (0x20-0x7E). A glyph outside that range renders as
 * a box; widen the range in the generator rather than working around it.
 */

#ifndef WAVEX_UI_FONTS_H
#define WAVEX_UI_FONTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t wavex_mono_14; /* JetBrains Mono Medium   */
extern const lv_font_t wavex_mono_18; /* JetBrains Mono Medium   */
extern const lv_font_t wavex_mono_26; /* JetBrains Mono SemiBold */
extern const lv_font_t wavex_mono_38; /* JetBrains Mono SemiBold */

#ifdef __cplusplus
}
#endif

#endif  // WAVEX_UI_FONTS_H
