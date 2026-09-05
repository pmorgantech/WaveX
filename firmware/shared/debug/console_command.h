// WaveX debug console grammar - shared verbatim by both MCUs.
//
// docs/features/debug-harness-and-hil.md §3. One line reader and one parser
// serve the Daisy CDC port and the ESP32 console UART, so the harness grammar
// cannot drift between boards: neither owns it, and it is host-tested here.
//
//   WAVEX-DBG <seq> <VERB> [args...]        host -> board
//   WAVEX-DBG: <seq> OK [key=value ...]     board -> host
//   WAVEX-DBG: <seq> ERR <reason>           board -> host
//
// <seq> is a host-chosen integer echoed back verbatim, which is what lets a
// test suite wait for *its* acknowledgement instead of sleeping and hoping.
// The legacy seq-less lines ("WAVEX-LOG ...", "WAVEX-FILTER ...") stay
// accepted for the human-driven scripts; they are reported as verb "LOG" /
// "FILTER" with seq = kNoSeq and get their legacy reply lines, no ack.
//
// Everything here is freestanding: no allocation, no exceptions, no I/O, and
// nothing that cannot run on the Daisy main loop. Parsing never happens in an
// ISR - the LineReader is the only part an ISR may touch, and it only stores
// bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace WaveX {
namespace Debug {

constexpr int32_t kNoSeq = -1;

/// Longest accepted line, including the "WAVEX-DBG " prefix. Sized for the
/// Daisy MSG verb's hex payload (128-byte payload = 256 hex chars) with room
/// for the header; a longer line is rejected whole, never truncated into a
/// shorter, valid-looking command.
constexpr size_t kMaxLineBytes = 320;

/// Verb token and argument bounds. Verbs are short upper-case words.
constexpr size_t kMaxVerbBytes = 16;

struct Command {
    int32_t seq = kNoSeq;           ///< echoed in the reply; kNoSeq = legacy line
    char verb[kMaxVerbBytes] = {};  ///< upper-cased, NUL-terminated
    const char* args = "";          ///< rest of the line after the verb, trimmed
    bool legacy = false;            ///< "WAVEX-<VERB> ..." rather than "WAVEX-DBG"
};

/// Accumulates console bytes into whole lines. Byte-at-a-time so the Daisy's
/// USB ISR can feed it; nothing is parsed until Take() on the consumer side.
///
/// A line is complete at '\n' or '\r'. Over-length input is discarded up to
/// the next terminator and reported as overflow, so a MSG payload that was
/// cut in half can never arrive as a shorter, valid-looking command. Empty
/// lines (a bare "\r\n" from a terminal) are ignored.
///
/// Single producer, single consumer: the producer calls Feed(); the consumer
/// polls Ready() and calls Take(). Ready() is published with release
/// semantics by the producer via the caller's own atomic flag - this class
/// keeps no atomics of its own so it stays usable from an ISR on the Daisy
/// and from a plain task on the ESP32 alike. While a line is held (Ready()
/// true and not yet Take()n) further bytes are dropped and counted, never
/// written over the line being parsed.
class LineReader {
   public:
    /// Returns true when a complete line became available on this byte.
    bool Feed(char c) {
        if (ready_) {
            ++dropped_;
            return false;
        }
        if (c == '\n' || c == '\r') {
            if (overflow_) {
                overflow_ = false;
                ++overflowed_lines_;
                len_ = 0;
                return false;
            }
            if (len_ == 0) {
                return false;
            }
            buf_[len_] = '\0';
            ready_ = true;
            return true;
        }
        if (overflow_) {
            return false;
        }
        if (len_ + 1 >= sizeof(buf_)) {
            overflow_ = true;
            len_ = 0;
            return false;
        }
        buf_[len_++] = c;
        return false;
    }

    bool Ready() const { return ready_; }

    /// The completed line. Valid until Release().
    const char* Line() const { return buf_; }

    /// Consumer is done with the line; the reader accepts bytes again.
    void Release() {
        ready_ = false;
        len_ = 0;
    }

    uint32_t DroppedBytes() const { return dropped_; }
    uint32_t OverflowedLines() const { return overflowed_lines_; }

   private:
    char buf_[kMaxLineBytes] = {};
    size_t len_ = 0;
    bool ready_ = false;
    bool overflow_ = false;
    uint32_t dropped_ = 0;
    uint32_t overflowed_lines_ = 0;
};

namespace detail {

inline const char* SkipSpaces(const char* p) {
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

inline bool StartsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

/// Copies one upper-cased word into `out`; returns the position after it.
inline const char* TakeVerb(const char* p, char* out, size_t cap) {
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t') {
        if (n + 1 < cap) {
            const char c = *p;
            out[n++] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
        }
        ++p;
    }
    out[n] = '\0';
    return p;
}

}  // namespace detail

/// Parses one console line. Returns false when the line is not a console
/// command at all (ordinary traffic); `out` is then untouched. A line that
/// starts like a command but is malformed returns true with an empty verb so
/// the caller can answer ERR rather than stay silent.
inline bool ParseCommand(const char* line, Command& out) {
    using namespace detail;
    if (!line) {
        return false;
    }
    // Anywhere in the line, not only at its start: the first line after a
    // port (re)opens can carry whatever bytes were already in flight, and a
    // terminal may prefix an echo. The verb still has to follow the marker.
    const char* p = std::strstr(line, "WAVEX-");
    if (!p) {
        return false;
    }
    p += 6;

    if (StartsWith(p, "DBG")) {
        p = SkipSpaces(p + 3);
        // <seq>
        bool neg = false;
        if (*p == '-') {
            neg = true;
            ++p;
        }
        if (*p < '0' || *p > '9') {
            out.seq = kNoSeq;
            out.verb[0] = '\0';
            out.args = "";
            out.legacy = false;
            return true;  // malformed: no sequence number
        }
        int32_t seq = 0;
        while (*p >= '0' && *p <= '9') {
            seq = seq * 10 + (*p - '0');
            if (seq > 99999999) {
                seq = 99999999;
            }
            ++p;
        }
        out.seq = neg ? -seq : seq;
        out.legacy = false;
        p = SkipSpaces(p);
        p = TakeVerb(p, out.verb, sizeof(out.verb));
        out.args = SkipSpaces(p);
        return true;
    }

    // Legacy: "WAVEX-<VERB> <args>" with no sequence number. "ENTER-DFU" is
    // matched by its own substring matcher on the Daisy before this ever
    // sees it; it is reported here only so a dispatcher can ignore it.
    out.seq = kNoSeq;
    out.legacy = true;
    p = TakeVerb(p, out.verb, sizeof(out.verb));
    out.args = SkipSpaces(p);
    return true;
}

/// Reply builders. Both write a complete line without the trailing newline
/// into `out` and return the length written (0 if `out` is too small for
/// even the header). Callers append their own key=value pairs to an OK with
/// AppendKv() so the on-wire form is one flat line the host can split on
/// spaces.
inline size_t FormatOk(int32_t seq, char* out, size_t cap) {
    const int n = std::snprintf(out, cap, "WAVEX-DBG: %ld OK", static_cast<long>(seq));
    return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
}

inline size_t FormatErr(int32_t seq, const char* reason, char* out, size_t cap) {
    const int n = std::snprintf(
        out, cap, "WAVEX-DBG: %ld ERR %s", static_cast<long>(seq), reason ? reason : "");
    return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
}

/// Appends " key=value" to a reply under construction. Values are written
/// verbatim, so callers must not pass values containing spaces (the host
/// splits on them); use AppendKvText for free text.
inline size_t AppendKv(char* out, size_t cap, size_t len, const char* key, const char* value) {
    if (cap == 0 || len + 1 >= cap) {
        return len;
    }
    // Formatted into a bounded scratch first, then copied by length: keeps
    // the truncation arithmetic in one place and the compiler's
    // format-truncation analysis quiet at every inlined call site.
    char pair[160];
    const int n = std::snprintf(pair, sizeof(pair), " %s=%s", key, value ? value : "");
    if (n <= 0) {
        return len;
    }
    size_t take = static_cast<size_t>(n);
    if (take >= sizeof(pair)) {
        take = sizeof(pair) - 1;
    }
    if (take > cap - 1 - len) {
        take = cap - 1 - len;
    }
    std::memcpy(out + len, pair, take);
    out[len + take] = '\0';
    return len + take;
}

inline size_t AppendKvInt(char* out, size_t cap, size_t len, const char* key, long value) {
    char v[16];
    std::snprintf(v, sizeof(v), "%ld", value);
    return AppendKv(out, cap, len, key, v);
}

/// Free text: spaces become '_' so the line still splits on spaces. Good
/// enough for labels and status lines a test compares by substring.
inline size_t AppendKvText(char* out, size_t cap, size_t len, const char* key, const char* text) {
    char v[96];
    size_t n = 0;
    for (const char* p = text ? text : ""; *p && n + 1 < sizeof(v); ++p) {
        v[n++] = (*p == ' ' || *p == '\t' || *p == '\n') ? '_' : *p;
    }
    v[n] = '\0';
    return AppendKv(out, cap, len, key, v);
}

/// Parses "<hex bytes>" (no separators, even length) into `out`. Returns the
/// byte count, or 0 on any malformed input or overflow.
inline size_t ParseHexBytes(const char* text, uint8_t* out, size_t cap) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    size_t n = 0;
    const char* p = text ? text : "";
    while (*p && *p != ' ') {
        const int hi = nib(p[0]);
        const int lo = p[1] ? nib(p[1]) : -1;
        if (hi < 0 || lo < 0 || n >= cap) {
            return 0;
        }
        out[n++] = static_cast<uint8_t>((hi << 4) | lo);
        p += 2;
    }
    return n;
}

/// Reads the next integer token from `*p`, advancing past it. Returns false
/// if no integer is there.
inline bool NextInt(const char** p, long* out) {
    const char* s = detail::SkipSpaces(*p);
    bool neg = false;
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        ++s;
    }
    if (*s < '0' || *s > '9') {
        return false;
    }
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    *out = neg ? -v : v;
    *p = s;
    return true;
}

/// Reads the next word token (upper-cased) from `*p`. Returns false if none.
inline bool NextWord(const char** p, char* out, size_t cap) {
    const char* s = detail::SkipSpaces(*p);
    if (*s == '\0') {
        out[0] = '\0';
        return false;
    }
    *p = detail::TakeVerb(s, out, cap);
    return true;
}

}  // namespace Debug
}  // namespace WaveX
