#pragma once

// Narrow SFZ v1 importer (docs/features/sfz-import.md). This module is HAL-free:
// it parses caller-provided lines, maps the supported opcode subset onto the
// existing Instrument/Zone model, resolves relative sample paths, and builds a
// deduplicated sample plan. FatFs and SampleMemMgr integration stay in the
// target-side sfz_loader.cpp so every operation here is host-testable.
//
// File convention used by the boot loader:
//   0:/wavex/sfz/<instrument>/<instrument>.sfz
// with sample= paths relative to the .sfz directory (and optional
// control/global default_path). A sample= value may contain spaces (the SFZ
// convention); it runs to the next token that is itself an opcode -
// whitespace, an identifier, '=' - or to a '<' header, whichever comes
// first. So "sample=saw mini.wav oscillator=on" is the path "saw mini.wav"
// followed by the opcode oscillator=on. A path containing "<" or a
// space-delimited "word=" cannot be expressed; nothing on real packs does.

#include "bss_static.hpp"
#include "instrument.hpp"
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace WaveX {
namespace AudioEngine {
namespace Sfz {

static constexpr size_t kMaxPath = 200;
static constexpr size_t kMaxLine = 512;
static constexpr size_t kMaxMacros = 16;
static constexpr size_t kMaxMacroName = 32;
static constexpr size_t kMaxMacroValue = 64;

enum class Error : uint8_t {
    None = 0,
    LineTooLong,
    InvalidValue,
    MissingSample,
    PathTooLong,
    InvalidRange,
    TooManyRegions,
    TooManyMacros,
    SampleTooLarge,
    BudgetExceeded,
};

struct Status {
    Error error = Error::None;
    uint32_t line = 0;
    uint16_t region_count = 0;
    uint16_t unknown_opcodes = 0;
    uint8_t item = 0;

    bool ok() const { return error == Error::None; }
};

template <typename T>
struct OptionalValue {
    T value{};
    bool present = false;
};

enum class LoopMode : uint8_t { Unset, NoLoop, OneShot, Continuous, Sustain };

// Parsed/cascaded opcodes for one region. Values remain independent of Zone so
// parsing and semantic conversion can be tested separately.
struct RegionOpcodes {
    char sample[kMaxPath] = {};
    char default_path[kMaxPath] = {};
    OptionalValue<int32_t> key;
    OptionalValue<int32_t> lokey;
    OptionalValue<int32_t> hikey;
    OptionalValue<int32_t> lovel;
    OptionalValue<int32_t> hivel;
    OptionalValue<int32_t> pitch_keycenter;
    OptionalValue<int32_t> transpose;
    OptionalValue<int32_t> tune;
    OptionalValue<float> volume_db;
    OptionalValue<float> pan;
    OptionalValue<int32_t> offset;
    OptionalValue<int32_t> end;
    OptionalValue<int32_t> loop_start;
    OptionalValue<int32_t> loop_end;
    OptionalValue<int32_t> group;
    OptionalValue<int32_t> off_by;
    OptionalValue<float> cutoff;
    OptionalValue<float> attack;
    OptionalValue<float> decay;
    OptionalValue<float> sustain_percent;
    OptionalValue<float> release;
    LoopMode loop_mode = LoopMode::Unset;
};

struct Document {
    RegionOpcodes regions[kMaxZones];
    uint16_t stored_regions = 0;
    uint16_t total_regions = 0;
    uint16_t unknown_opcodes = 0;
};

namespace detail {

inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline char Lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool Equal(const char* a, const char* b) {
    return a && b && std::strcmp(a, b) == 0;
}

inline bool Copy(char* dst, size_t capacity, const char* src, size_t length) {
    if (!dst || capacity == 0 || !src || length >= capacity) {
        return false;
    }
    std::memcpy(dst, src, length);
    dst[length] = '\0';
    return true;
}

inline bool Copy(char* dst, size_t capacity, const char* src) {
    return src && Copy(dst, capacity, src, std::strlen(src));
}

inline const char* SkipSpace(const char* p) {
    while (p && IsSpace(*p)) {
        ++p;
    }
    return p;
}

inline bool ParseInt(const char* text, int32_t& out) {
    if (!text || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    end = const_cast<char*>(SkipSpace(end));
    if (!end || *end != '\0' || errno == ERANGE || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    out = static_cast<int32_t>(value);
    return true;
}

// Plain decimal: [ws][sign]digits[.digits][(e|E)[sign]digits][ws]. That is
// every number an SFZ opcode carries. Deliberately NOT strtof(): newlib's
// strtof is the full correctly-rounded strtod with hex-float and inf/nan
// parsing, which linked ~8 KB (_strtod_l, __gethex, the __mprec big-integer
// suite) and, through _Balloc, put malloc on the import path. Accuracy here
// is ~1e-7 relative - the mantissa is exact to 9 digits and the scaling is a
// double multiply - against opcode values quantised to cents, dB/10, and
// milliseconds.
inline bool ParseFloat(const char* text, float& out) {
    const char* p = SkipSpace(text);
    if (!p || *p == '\0') {
        return false;
    }
    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        ++p;
    }

    // Mantissa as an integer, exact while it fits 9 digits; digits past that
    // only shift the exponent so a long fraction cannot overflow it.
    uint32_t mantissa = 0;
    int exponent = 0;
    int digits = 0;
    int significant = 0;
    for (; *p >= '0' && *p <= '9'; ++p, ++digits) {
        if (significant < 9) {
            mantissa = mantissa * 10u + static_cast<uint32_t>(*p - '0');
            if (mantissa != 0) {
                ++significant;
            }
        } else {
            ++exponent;
        }
    }
    if (*p == '.') {
        ++p;
        for (; *p >= '0' && *p <= '9'; ++p, ++digits) {
            if (significant < 9) {
                mantissa = mantissa * 10u + static_cast<uint32_t>(*p - '0');
                if (mantissa != 0) {
                    ++significant;
                }
                --exponent;
            }
        }
    }
    if (digits == 0) {
        return false;
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        bool exp_negative = false;
        if (*p == '+' || *p == '-') {
            exp_negative = (*p == '-');
            ++p;
        }
        if (*p < '0' || *p > '9') {
            return false;
        }
        int exp_value = 0;
        for (; *p >= '0' && *p <= '9'; ++p) {
            if (exp_value < 1000) {  // saturate; anything this large is out of float range
                exp_value = exp_value * 10 + (*p - '0');
            }
        }
        exponent += exp_negative ? -exp_value : exp_value;
    }
    p = SkipSpace(p);
    if (*p != '\0') {
        return false;
    }

    if (mantissa == 0) {
        out = negative ? -0.0f : 0.0f;
        return true;
    }
    // float range is ~1e-45..3.4e38; a 9-digit mantissa moves that window by
    // at most 9. Anything past +-60 cannot be finite, so bail before the
    // scaling loop below does 1000 multiplies for nothing.
    if (exponent > 60 || exponent < -60) {
        return false;
    }
    double value = static_cast<double>(mantissa);
    double scale = 1.0;
    for (int i = 0; i < (exponent < 0 ? -exponent : exponent); ++i) {
        scale *= 10.0;
    }
    value = exponent < 0 ? value / scale : value * scale;
    const float result = static_cast<float>(negative ? -value : value);
    if (!std::isfinite(result)) {
        return false;
    }
    out = result;
    return true;
}

inline int32_t ClampInt(int32_t value, int32_t lo, int32_t hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

inline float ClampFloat(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

inline bool ParseKey(const char* text, int32_t& out) {
    if (ParseInt(text, out)) {
        return true;
    }
    if (!text || *text == '\0') {
        return false;
    }

    const char note = Lower(*text++);
    int pitch_class = -1;
    switch (note) {
        case 'c':
            pitch_class = 0;
            break;
        case 'd':
            pitch_class = 2;
            break;
        case 'e':
            pitch_class = 4;
            break;
        case 'f':
            pitch_class = 5;
            break;
        case 'g':
            pitch_class = 7;
            break;
        case 'a':
            pitch_class = 9;
            break;
        case 'b':
            pitch_class = 11;
            break;
        default:
            return false;
    }
    if (*text == '#' || *text == 'b' || *text == 'B') {
        pitch_class += (*text == '#') ? 1 : -1;
        ++text;
    }
    int32_t octave = 0;
    if (!ParseInt(text, octave)) {
        return false;
    }
    // Bound before arithmetic: a malicious octave near INT32_MAX would make
    // (octave + 1) * 12 signed-overflow before the mapper could clamp it.
    if (octave < -2 || octave > 10) {
        return false;
    }
    out = (octave + 1) * 12 + pitch_class;  // SFZ convention: c4 == MIDI 60.
    return true;
}

inline bool SetInt(OptionalValue<int32_t>& field, const char* value, bool is_key = false) {
    int32_t parsed = 0;
    if (!(is_key ? ParseKey(value, parsed) : ParseInt(value, parsed))) {
        return false;
    }
    field.value = parsed;
    field.present = true;
    return true;
}

inline bool SetFloat(OptionalValue<float>& field, const char* value) {
    float parsed = 0.0f;
    if (!ParseFloat(value, parsed)) {
        return false;
    }
    field.value = parsed;
    field.present = true;
    return true;
}

inline bool AppendPath(char* out, size_t capacity, const char* part, bool add_separator) {
    size_t used = std::strlen(out);
    if (add_separator && used != 0 && out[used - 1] != '/' && out[used - 1] != '\\') {
        if (used + 1 >= capacity) {
            return false;
        }
        out[used++] = '/';
        out[used] = '\0';
    }
    const size_t length = std::strlen(part);
    if (used + length >= capacity) {
        return false;
    }
    std::memcpy(out + used, part, length + 1);
    return true;
}

inline void NormalizePath(char* path) {
    if (!path) {
        return;
    }
    size_t read = 0;
    size_t write = 0;
    while (path[read] != '\0') {
        char c = path[read++];
        if (c == '\\') {
            c = '/';
        }
        if (c == '/' && write > 0 && path[write - 1] == '/') {
            continue;
        }
        path[write++] = c;
    }
    path[write] = '\0';
}

inline bool ResolvePath(const char* sfz_path,
                        const char* default_path,
                        const char* sample,
                        char* out,
                        size_t capacity) {
    if (!sfz_path || !sample || *sample == '\0' || !out || capacity == 0) {
        return false;
    }
    out[0] = '\0';

    // A drive-qualified or root-qualified sample path is already absolute.
    if ((sample[0] != '\0' && sample[1] == ':') || sample[0] == '/' || sample[0] == '\\') {
        if (!Copy(out, capacity, sample)) {
            return false;
        }
        NormalizePath(out);
        return true;
    }

    const char* slash = std::strrchr(sfz_path, '/');
    const char* backslash = std::strrchr(sfz_path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
    const size_t directory_length = slash ? static_cast<size_t>(slash - sfz_path) : 0;
    if (directory_length != 0 && !Copy(out, capacity, sfz_path, directory_length)) {
        return false;
    }
    if (default_path && *default_path != '\0' &&
        !AppendPath(out, capacity, default_path, directory_length != 0)) {
        return false;
    }
    if (!AppendPath(out, capacity, sample, out[0] != '\0')) {
        return false;
    }
    NormalizePath(out);
    return true;
}

}  // namespace detail

class Parser {
   public:
    void Reset() {
        // In place: `document_ = Document{}` built a 20 KB temporary on the
        // stack first (bss_static.hpp).
        WaveX::ReconstructInPlace(document_);
        global_ = RegionOpcodes{};
        group_ = RegionOpcodes{};
        current_ = RegionOpcodes{};
        scope_ = Scope::Ignore;
        group_active_ = false;
        region_active_ = false;
        macro_count_ = 0;
        status_ = Status{};
    }

    bool FeedLine(const char* line, uint32_t line_number) {
        if (!status_.ok()) {
            return false;
        }
        const size_t length = line ? std::strlen(line) : 0;
        if (!line || length >= kMaxLine) {
            return Fail(Error::LineTooLong, line_number);
        }
        detail::Copy(work_, sizeof(work_), line, length);
        char* comment = std::strstr(work_, "//");
        if (comment) {
            *comment = '\0';
        }
        TrimRight(work_);

        const char* p = detail::SkipSpace(work_);
        if (std::strncmp(p, "#define", 7) == 0 && detail::IsSpace(p[7])) {
            return ParseDefine(p + 7, line_number);
        }

        while (p && *p != '\0') {
            p = detail::SkipSpace(p);
            if (*p == '\0') {
                break;
            }
            if (*p == '<') {
                const char* close = std::strchr(p, '>');
                if (!close) {
                    return Fail(Error::InvalidValue, line_number);
                }
                char header[16];
                const size_t header_length = static_cast<size_t>(close - p - 1);
                if (!detail::Copy(header, sizeof(header), p + 1, header_length)) {
                    return Fail(Error::InvalidValue, line_number);
                }
                Lowercase(header);
                if (!HandleHeader(header, line_number)) {
                    return false;
                }
                p = close + 1;
                continue;
            }

            char key[32];
            size_t key_length = 0;
            while (p[key_length] != '\0' && p[key_length] != '=' &&
                   !detail::IsSpace(p[key_length]) && p[key_length] != '<') {
                ++key_length;
            }
            if (p[key_length] != '=') {
                while (*p != '\0' && !detail::IsSpace(*p) && *p != '<') {
                    ++p;
                }
                ++document_.unknown_opcodes;
                continue;
            }
            if (!detail::Copy(key, sizeof(key), p, key_length)) {
                return Fail(Error::InvalidValue, line_number);
            }
            Lowercase(key);
            p += key_length + 1;

            const char* value_start = p;
            const char* value_end = p;
            if (detail::Equal(key, "sample")) {
                value_end = SamplePathEnd(p);
                p = value_end;
            } else {
                while (*value_end != '\0' && !detail::IsSpace(*value_end) && *value_end != '<') {
                    ++value_end;
                }
                p = value_end;
            }
            while (value_end > value_start && detail::IsSpace(value_end[-1])) {
                --value_end;
            }
            char value[kMaxPath];
            if (!detail::Copy(value,
                              sizeof(value),
                              value_start,
                              static_cast<size_t>(value_end - value_start))) {
                return Fail(Error::PathTooLong, line_number);
            }
            StripMatchingQuotes(value);
            char expanded[kMaxPath];
            if (!ExpandMacro(value, expanded, sizeof(expanded))) {
                return Fail(Error::InvalidValue, line_number);
            }
            if (!ApplyOpcode(key, expanded, line_number)) {
                return false;
            }
        }
        return true;
    }

    bool Finish() {
        if (!status_.ok()) {
            return false;
        }
        if (!CommitRegion(status_.line)) {
            return false;
        }
        status_.region_count = document_.total_regions;
        status_.unknown_opcodes = document_.unknown_opcodes;
        if (document_.total_regions > kMaxZones) {
            return Fail(Error::TooManyRegions, status_.line);
        }
        return true;
    }

    const Document& GetDocument() const { return document_; }
    const Status& GetStatus() const { return status_; }

   private:
    enum class Scope : uint8_t { Ignore, Global, Group, Region, Control };

    struct Macro {
        char name[kMaxMacroName] = {};
        char value[kMaxMacroValue] = {};
    };

    bool Fail(Error error, uint32_t line) {
        status_.error = error;
        status_.line = line;
        status_.region_count = document_.total_regions;
        status_.unknown_opcodes = document_.unknown_opcodes;
        return false;
    }

    static void Lowercase(char* text) {
        for (; text && *text; ++text) {
            *text = detail::Lower(*text);
        }
    }

    static void TrimRight(char* text) {
        if (!text) {
            return;
        }
        size_t length = std::strlen(text);
        while (length > 0 && detail::IsSpace(text[length - 1])) {
            text[--length] = '\0';
        }
    }

    // Where a sample= value ends: at a '<' (a header on the same line) or
    // just before the first "<space>identifier=" token, which is the next
    // opcode. Spaces inside the path survive; "word=" inside a path does
    // not, which real packs never do.
    static const char* SamplePathEnd(const char* p) {
        const char* q = p;
        while (*q != '\0' && *q != '<') {
            if (detail::IsSpace(*q)) {
                const char* t = q;
                while (detail::IsSpace(*t)) {
                    ++t;
                }
                const char* id = t;
                while ((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') ||
                       (*id >= '0' && *id <= '9') || *id == '_') {
                    ++id;
                }
                if (id > t && *id == '=') {
                    return q;
                }
                q = t > q ? t : q + 1;
                continue;
            }
            ++q;
        }
        return q;
    }

    static void StripMatchingQuotes(char* text) {
        const size_t length = text ? std::strlen(text) : 0;
        if (length >= 2 && ((text[0] == '"' && text[length - 1] == '"') ||
                            (text[0] == '\'' && text[length - 1] == '\''))) {
            std::memmove(text, text + 1, length - 2);
            text[length - 2] = '\0';
        }
    }

    bool ParseDefine(const char* text, uint32_t line) {
        text = detail::SkipSpace(text);
        if (!text || *text != '$') {
            return Fail(Error::InvalidValue, line);
        }
        const char* name_end = text;
        while (*name_end != '\0' && !detail::IsSpace(*name_end)) {
            ++name_end;
        }
        const char* value = detail::SkipSpace(name_end);
        if (*value == '\0') {
            return Fail(Error::InvalidValue, line);
        }
        if (macro_count_ >= kMaxMacros) {
            return Fail(Error::TooManyMacros, line);
        }
        Macro& macro = macros_[macro_count_++];
        if (!detail::Copy(
                macro.name, sizeof(macro.name), text, static_cast<size_t>(name_end - text)) ||
            !detail::Copy(macro.value, sizeof(macro.value), value)) {
            return Fail(Error::InvalidValue, line);
        }
        return true;
    }

    bool ExpandMacro(const char* value, char* out, size_t capacity) const {
        if (!out || capacity == 0) {
            return false;
        }
        out[0] = '\0';
        const char* read = value ? value : "";
        size_t written = 0;
        while (*read != '\0') {
            const Macro* match = nullptr;
            size_t match_length = 0;
            if (*read == '$') {
                for (size_t i = 0; i < macro_count_; ++i) {
                    const size_t name_length = std::strlen(macros_[i].name);
                    if (name_length >= match_length &&
                        std::strncmp(read, macros_[i].name, name_length) == 0) {
                        // Longest name wins ($ROOT before $R); a later define
                        // wins when the names are identical. Replacement text
                        // is copied verbatim, so expansion is intentionally
                        // non-recursive as required by the v1 grammar.
                        match = &macros_[i];
                        match_length = name_length;
                    }
                }
            }

            const char* replacement = match ? match->value : read;
            const size_t replacement_length = match ? std::strlen(replacement) : 1;
            if (written + replacement_length >= capacity) {
                return false;
            }
            std::memcpy(out + written, replacement, replacement_length);
            written += replacement_length;
            read += match ? match_length : 1;
        }
        out[written] = '\0';
        return true;
    }

    bool HandleHeader(const char* header, uint32_t line) {
        if (!CommitRegion(line)) {
            return false;
        }
        if (detail::Equal(header, "global")) {
            scope_ = Scope::Global;
            group_active_ = false;
        } else if (detail::Equal(header, "group")) {
            group_ = global_;
            scope_ = Scope::Group;
            group_active_ = true;
        } else if (detail::Equal(header, "region")) {
            current_ = group_active_ ? group_ : global_;
            scope_ = Scope::Region;
            region_active_ = true;
        } else if (detail::Equal(header, "control")) {
            scope_ = Scope::Control;
            group_active_ = false;
        } else {
            scope_ = Scope::Ignore;
            group_active_ = false;
        }
        return true;
    }

    bool CommitRegion(uint32_t line) {
        if (!region_active_) {
            return true;
        }
        ++document_.total_regions;
        if (document_.stored_regions < kMaxZones) {
            document_.regions[document_.stored_regions++] = current_;
        }
        region_active_ = false;
        status_.line = line;
        return true;
    }

    RegionOpcodes* TargetFor(const char* key) {
        switch (scope_) {
            case Scope::Global:
                return &global_;
            case Scope::Group:
                return &group_;
            case Scope::Region:
                return &current_;
            case Scope::Control:
                return detail::Equal(key, "default_path") ? &global_ : nullptr;
            case Scope::Ignore:
                return nullptr;
        }
        return nullptr;
    }

    bool ApplyOpcode(const char* key, const char* value, uint32_t line) {
        RegionOpcodes* target = TargetFor(key);
        if (!target) {
            ++document_.unknown_opcodes;
            return true;
        }

        bool valid = true;
        bool known = true;
        if (detail::Equal(key, "sample")) {
            valid = detail::Copy(target->sample, sizeof(target->sample), value);
        } else if (detail::Equal(key, "default_path")) {
            valid = detail::Copy(target->default_path, sizeof(target->default_path), value);
        } else if (detail::Equal(key, "key")) {
            valid = detail::SetInt(target->key, value, true);
        } else if (detail::Equal(key, "lokey")) {
            valid = detail::SetInt(target->lokey, value, true);
        } else if (detail::Equal(key, "hikey")) {
            valid = detail::SetInt(target->hikey, value, true);
        } else if (detail::Equal(key, "lovel")) {
            valid = detail::SetInt(target->lovel, value);
        } else if (detail::Equal(key, "hivel")) {
            valid = detail::SetInt(target->hivel, value);
        } else if (detail::Equal(key, "pitch_keycenter")) {
            valid = detail::SetInt(target->pitch_keycenter, value, true);
        } else if (detail::Equal(key, "transpose")) {
            valid = detail::SetInt(target->transpose, value);
        } else if (detail::Equal(key, "tune")) {
            valid = detail::SetInt(target->tune, value);
        } else if (detail::Equal(key, "volume")) {
            valid = detail::SetFloat(target->volume_db, value);
        } else if (detail::Equal(key, "pan")) {
            valid = detail::SetFloat(target->pan, value);
        } else if (detail::Equal(key, "offset")) {
            valid = detail::SetInt(target->offset, value);
        } else if (detail::Equal(key, "end")) {
            valid = detail::SetInt(target->end, value);
        } else if (detail::Equal(key, "loop_start")) {
            valid = detail::SetInt(target->loop_start, value);
        } else if (detail::Equal(key, "loop_end")) {
            valid = detail::SetInt(target->loop_end, value);
        } else if (detail::Equal(key, "group")) {
            valid = detail::SetInt(target->group, value);
        } else if (detail::Equal(key, "off_by")) {
            valid = detail::SetInt(target->off_by, value);
        } else if (detail::Equal(key, "cutoff")) {
            valid = detail::SetFloat(target->cutoff, value);
        } else if (detail::Equal(key, "ampeg_attack")) {
            valid = detail::SetFloat(target->attack, value);
        } else if (detail::Equal(key, "ampeg_decay")) {
            valid = detail::SetFloat(target->decay, value);
        } else if (detail::Equal(key, "ampeg_sustain")) {
            valid = detail::SetFloat(target->sustain_percent, value);
        } else if (detail::Equal(key, "ampeg_release")) {
            valid = detail::SetFloat(target->release, value);
        } else if (detail::Equal(key, "loop_mode")) {
            if (detail::Equal(value, "no_loop")) {
                target->loop_mode = LoopMode::NoLoop;
            } else if (detail::Equal(value, "one_shot")) {
                target->loop_mode = LoopMode::OneShot;
            } else if (detail::Equal(value, "loop_continuous")) {
                target->loop_mode = LoopMode::Continuous;
            } else if (detail::Equal(value, "loop_sustain")) {
                target->loop_mode = LoopMode::Sustain;
            } else {
                valid = false;
            }
        } else {
            known = false;
        }

        if (!known) {
            ++document_.unknown_opcodes;
            return true;
        }
        return valid ? true : Fail(Error::InvalidValue, line);
    }

    Document document_{};
    RegionOpcodes global_{};
    RegionOpcodes group_{};
    RegionOpcodes current_{};
    Scope scope_ = Scope::Ignore;
    bool group_active_ = false;
    bool region_active_ = false;
    Macro macros_[kMaxMacros]{};
    size_t macro_count_ = 0;
    Status status_{};
    char work_[kMaxLine]{};
};

struct MappedInstrument {
    Instrument instrument;
    char sample_paths[kMaxZones][kMaxPath] = {};
    uint8_t zone_count = 0;
};

inline bool MapDocument(const Document& document,
                        const char* sfz_path,
                        MappedInstrument& out,
                        Status& status) {
    WaveX::ReconstructInPlace(out);  // not `out = MappedInstrument{}`: 8 KB stack temporary
    out.instrument.origin = InstrumentOrigin::SfzImport;
    status = Status{};
    status.region_count = document.total_regions;
    status.unknown_opcodes = document.unknown_opcodes;
    if (document.total_regions > kMaxZones) {
        status.error = Error::TooManyRegions;
        return false;
    }

    for (uint16_t i = 0; i < document.stored_regions; ++i) {
        const RegionOpcodes& source = document.regions[i];
        status.item = static_cast<uint8_t>(i);
        if (source.end.present && source.end.value == -1) {
            continue;  // SFZ's explicit disabled-region sentinel.
        }
        if (source.sample[0] == '\0') {
            status.error = Error::MissingSample;
            return false;
        }

        Zone zone;
        if (source.key.present) {
            const int32_t key = detail::ClampInt(source.key.value, 0, 127);
            zone.key_lo = static_cast<uint8_t>(key);
            zone.key_hi = static_cast<uint8_t>(key);
        } else {
            if (source.lokey.present) {
                zone.key_lo = static_cast<uint8_t>(detail::ClampInt(source.lokey.value, 0, 127));
            }
            if (source.hikey.present) {
                zone.key_hi = static_cast<uint8_t>(detail::ClampInt(source.hikey.value, 0, 127));
            }
        }
        if (source.lovel.present) {
            zone.vel_lo = static_cast<uint8_t>(detail::ClampInt(source.lovel.value, 1, 127));
        }
        if (source.hivel.present) {
            zone.vel_hi = static_cast<uint8_t>(detail::ClampInt(source.hivel.value, 1, 127));
        }
        if (zone.key_lo > zone.key_hi || zone.vel_lo > zone.vel_hi) {
            status.error = Error::InvalidRange;
            return false;
        }

        int32_t root = 60;
        if (source.pitch_keycenter.present) {
            root = source.pitch_keycenter.value;
        } else if (source.key.present) {
            root = source.key.value;
        } else if (source.lokey.present) {
            root = source.lokey.value;
        }
        zone.root_note = static_cast<uint8_t>(detail::ClampInt(root, 0, 127));
        if (source.transpose.present) {
            zone.coarse_tune =
                static_cast<int8_t>(detail::ClampInt(source.transpose.value, -48, 48));
        }
        if (source.tune.present) {
            zone.fine_tune = static_cast<int8_t>(detail::ClampInt(source.tune.value, -100, 100));
        }
        if (source.volume_db.present) {
            const float db = detail::ClampFloat(source.volume_db.value, -144.0f, 24.0f);
            zone.gain = std::pow(10.0f, db / 20.0f);
        }
        if (source.pan.present) {
            zone.pan = detail::ClampFloat((source.pan.value + 100.0f) / 200.0f, 0.0f, 1.0f);
        }
        if (source.offset.present) {
            zone.start_frame =
                static_cast<uint32_t>(source.offset.value < 0 ? 0 : source.offset.value);
        }
        if (source.end.present && source.end.value >= 0) {
            zone.end_frame = static_cast<uint32_t>(source.end.value);
        }
        if (source.loop_start.present) {
            zone.loop_start =
                static_cast<uint32_t>(source.loop_start.value < 0 ? 0 : source.loop_start.value);
        }
        if (source.loop_end.present) {
            zone.loop_end =
                static_cast<uint32_t>(source.loop_end.value < 0 ? 0 : source.loop_end.value);
        }
        if (source.loop_mode == LoopMode::Continuous || source.loop_mode == LoopMode::Sustain) {
            zone.loop_mode = ZONE_LOOP_FORWARD;
        } else if (source.loop_mode == LoopMode::NoLoop) {
            zone.loop_mode = ZONE_LOOP_OFF;
        } else if (source.loop_mode == LoopMode::OneShot) {
            zone.flags |= ZONE_FLAG_ONE_SHOT;
        }
        if (source.group.present && source.group.value > 0) {
            zone.choke_group = static_cast<uint8_t>(detail::ClampInt(source.group.value, 1, 255));
        }
        // Asymmetric off_by cannot be represented by Zone. The common
        // group=N/off_by=N case already collapses to the symmetric group above.
        if (source.cutoff.present) {
            zone.cutoff_hz = detail::ClampFloat(source.cutoff.value, 0.0f, 20000.0f);
        }
        if (source.attack.present) {
            zone.attack_s = detail::ClampFloat(source.attack.value, 0.0f, 60.0f);
        }
        if (source.decay.present) {
            zone.decay_s = detail::ClampFloat(source.decay.value, 0.0f, 60.0f);
        }
        if (source.sustain_percent.present) {
            zone.sustain = detail::ClampFloat(source.sustain_percent.value / 100.0f, 0.0f, 1.0f);
        }
        if (source.release.present) {
            zone.release_s = detail::ClampFloat(source.release.value, 0.0f, 60.0f);
        }
        zone.in_use = true;

        const uint8_t destination = out.zone_count;
        if (!detail::ResolvePath(sfz_path,
                                 source.default_path,
                                 source.sample,
                                 out.sample_paths[destination],
                                 sizeof(out.sample_paths[destination]))) {
            status.error = Error::PathTooLong;
            return false;
        }
        out.instrument.zones[destination] = zone;
        ++out.zone_count;
    }
    return true;
}

struct SamplePlanEntry {
    // Index of the first mapped zone that names this path. Keep the plan as an
    // index rather than duplicating up to 32 x 200 path bytes in permanent
    // SRAM; MappedInstrument stays alive for the boot load.
    uint8_t path_zone = 0;
    uint16_t sample_id = 0;
};

struct SamplePlan {
    SamplePlanEntry entries[kMaxZones];
    uint8_t count = 0;
};

inline bool BuildSamplePlan(MappedInstrument& mapped, SamplePlan& plan, Status& status) {
    plan = SamplePlan{};
    for (uint8_t zone_index = 0; zone_index < mapped.zone_count; ++zone_index) {
        uint8_t entry_index = 0;
        for (; entry_index < plan.count; ++entry_index) {
            if (detail::Equal(mapped.sample_paths[plan.entries[entry_index].path_zone],
                              mapped.sample_paths[zone_index])) {
                break;
            }
        }
        if (entry_index == plan.count) {
            if (plan.count >= kMaxZones) {
                status.error = Error::TooManyRegions;
                return false;
            }
            SamplePlanEntry& entry = plan.entries[plan.count];
            entry.path_zone = zone_index;
            entry.sample_id = static_cast<uint16_t>(plan.count + 1u);
            ++plan.count;
        }
        mapped.instrument.zones[zone_index].sample_id = plan.entries[entry_index].sample_id;
    }
    return true;
}

struct SampleProbe {
    uint32_t bytes = 0;
};

inline bool ValidateSampleBudget(const SampleProbe* probes,
                                 uint8_t count,
                                 uint32_t free_bytes,
                                 uint32_t reserve_bytes,
                                 uint32_t per_sample_max,
                                 uint32_t& total_bytes,
                                 Status& status) {
    total_bytes = 0;
    for (uint8_t i = 0; i < count; ++i) {
        status.item = i;
        if (probes[i].bytes == 0 || probes[i].bytes > per_sample_max) {
            status.error = Error::SampleTooLarge;
            return false;
        }
        if (UINT32_MAX - total_bytes < probes[i].bytes) {
            status.error = Error::BudgetExceeded;
            return false;
        }
        total_bytes += probes[i].bytes;
    }
    const uint32_t usable = free_bytes > reserve_bytes ? free_bytes - reserve_bytes : 0;
    if (total_bytes > usable) {
        status.error = Error::BudgetExceeded;
        return false;
    }
    return true;
}

}  // namespace Sfz
}  // namespace AudioEngine
}  // namespace WaveX
