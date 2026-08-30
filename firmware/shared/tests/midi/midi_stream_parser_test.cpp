// Host tests for the serial-MIDI byte-stream parser (roadmap Phase 1
// item 8). Exercises the byte-level grammar edge cases that break naive
// parsers on real cables: running status, real-time interleave, SysEx,
// system-common alignment, orphan data bytes, velocity-0 NoteOn.

#include "midi/midi_stream_parser.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using WaveX::Midi::Event;
using WaveX::Midi::EventType;
using WaveX::Midi::StreamParser;

namespace {

// Feeds every byte and collects the emitted events.
std::vector<Event> FeedAll(StreamParser& parser, const std::vector<uint8_t>& bytes) {
    std::vector<Event> events;
    Event ev;
    for (uint8_t b: bytes) {
        if (parser.Feed(b, ev)) {
            events.push_back(ev);
        }
    }
    return events;
}

TEST(MidiStreamParser, NoteOnBasic) {
    StreamParser p;
    auto events = FeedAll(p, {0x90, 60, 100});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].channel, 0);
    EXPECT_EQ(events[0].data1, 60);
    EXPECT_EQ(events[0].data2, 100);
}

TEST(MidiStreamParser, NoteOffBasic) {
    StreamParser p;
    auto events = FeedAll(p, {0x83, 72, 64});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOff);
    EXPECT_EQ(events[0].channel, 3);
    EXPECT_EQ(events[0].data1, 72);
    EXPECT_EQ(events[0].data2, 64);
}

TEST(MidiStreamParser, NoteOnVelocityZeroIsNoteOff) {
    StreamParser p;
    auto events = FeedAll(p, {0x91, 60, 0});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOff);
    EXPECT_EQ(events[0].channel, 1);
    EXPECT_EQ(events[0].data1, 60);
    EXPECT_EQ(events[0].data2, 0);
}

TEST(MidiStreamParser, RunningStatusEmitsConsecutiveNotes) {
    StreamParser p;
    // One status byte, three note-on payloads (third is velocity-0 = off).
    auto events = FeedAll(p, {0x90, 60, 100, 64, 90, 60, 0});
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 60);
    EXPECT_EQ(events[1].type, EventType::NoteOn);
    EXPECT_EQ(events[1].data1, 64);
    EXPECT_EQ(events[2].type, EventType::NoteOff);
    EXPECT_EQ(events[2].data1, 60);
}

TEST(MidiStreamParser, RealTimeBytesInterleaveMidMessage) {
    StreamParser p;
    // 0xF8 (clock) and 0xFE (active sensing) injected between status and
    // data, and between the two data bytes - message must still assemble.
    auto events = FeedAll(p, {0x90, 0xF8, 60, 0xFE, 100});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 60);
    EXPECT_EQ(events[0].data2, 100);
}

TEST(MidiStreamParser, SysExPayloadIsSkipped) {
    StreamParser p;
    // SysEx with payload bytes that would look like note data, then a real
    // note-on after EOX.
    auto events = FeedAll(p, {0xF0, 0x7E, 60, 100, 0x41, 0xF7, 0x90, 62, 90});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 62);
}

TEST(MidiStreamParser, StatusByteTerminatesDanglingSysEx) {
    StreamParser p;
    // Malformed stream: SysEx never sees its 0xF7. A new status byte must
    // still recover the parser.
    auto events = FeedAll(p, {0xF0, 0x7E, 0x01, 0x90, 60, 100});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 60);
}

TEST(MidiStreamParser, RealTimeInsideSysExDoesNotTerminateIt) {
    StreamParser p;
    auto events = FeedAll(p, {0xF0, 0x7E, 0xF8, 60, 100, 0xF7, 0x80, 60, 0});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOff);
}

TEST(MidiStreamParser, SystemCommonCancelsRunningStatus) {
    StreamParser p;
    // Song position pointer (0xF2 + 2 data bytes) cancels running status;
    // the following orphan data bytes must be discarded, not misparsed as
    // a note-on payload.
    auto events = FeedAll(p, {0x90, 60, 100, 0xF2, 0x10, 0x20, 62, 90});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data1, 60);
}

TEST(MidiStreamParser, SystemCommonDataBytesStayAligned) {
    StreamParser p;
    // MTC quarter frame (0xF1 + 1 data byte) directly followed by a full
    // note-on - the 0x05 must be consumed as F1 payload.
    auto events = FeedAll(p, {0xF1, 0x05, 0x90, 60, 100});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 60);
}

TEST(MidiStreamParser, OrphanDataBytesBeforeAnyStatusAreDiscarded) {
    StreamParser p;
    auto events = FeedAll(p, {60, 100, 0x23, 0x90, 61, 99});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data1, 61);
}

TEST(MidiStreamParser, TwoByteMessagesKeepAlignment) {
    StreamParser p;
    // Program change (0xC0, 1 data byte) then channel pressure (0xD0,
    // 1 data byte) - neither is reported, but a following note must parse.
    auto events = FeedAll(p, {0xC0, 5, 0xD2, 100, 0x90, 60, 100});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
}

TEST(MidiStreamParser, ThreeByteUnreportedMessagesKeepAlignment) {
    StreamParser p;
    // Pitch bend (0xE0) and poly aftertouch (0xA0) are not reported but
    // their payloads must be consumed - including via running status.
    auto events = FeedAll(p, {0xE0, 0x00, 0x40, 0x12, 0x34, 0xA1, 60, 50, 0x90, 60, 100});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
}

TEST(MidiStreamParser, ControlChangeEmitted) {
    StreamParser p;
    auto events = FeedAll(p, {0xB2, 74, 101});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::ControlChange);
    EXPECT_EQ(events[0].channel, 2);
    EXPECT_EQ(events[0].data1, 74);
    EXPECT_EQ(events[0].data2, 101);
}

TEST(MidiStreamParser, ChannelExtractedFromStatus) {
    StreamParser p;
    auto events = FeedAll(p, {0x9F, 60, 100, 0x8F, 60, 0});
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].channel, 15);
    EXPECT_EQ(events[1].channel, 15);
}

TEST(MidiStreamParser, ResetClearsRunningStatusAndSysEx) {
    StreamParser p;
    Event ev;
    // Mid-message reset: 0x90 60 [reset] 100 must not emit.
    EXPECT_FALSE(p.Feed(0x90, ev));
    EXPECT_FALSE(p.Feed(60, ev));
    p.Reset();
    EXPECT_FALSE(p.Feed(100, ev));  // orphan data byte after reset
    // And a clean message afterwards works.
    auto events = FeedAll(p, {0x90, 61, 100});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data1, 61);
}

TEST(MidiStreamParser, InterleavedRunningStatusStream) {
    StreamParser p;
    // A realistic dense stream: running-status notes with clock ticks
    // sprinkled everywhere.
    auto events = FeedAll(p, {0xF8, 0x90, 36, 127, 0xF8, 38, 110, 0xF8, 36, 0, 38, 0, 0xF8, 0xF8});
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[1].type, EventType::NoteOn);
    EXPECT_EQ(events[2].type, EventType::NoteOff);
    EXPECT_EQ(events[3].type, EventType::NoteOff);
}

// A System Common message whose data bytes never arrive - a glitched cable, a
// peer reset mid-message - must not eat the next message's data. The parser
// used to keep the outstanding count across the following status byte, so the
// note below lost its number and velocity and never sounded.
TEST(MidiStreamParser, TruncatedSystemCommonDoesNotSwallowTheNextMessage) {
    StreamParser parser;
    // F2 (Song Position) promises two data bytes; only the status arrives.
    auto events = FeedAll(parser, {0xF2, 0x90, 0x3C, 0x64});

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 0x3C);
    EXPECT_EQ(events[0].data2, 0x64);
}

// The same, with the System Common cut short by SysEx rather than by a
// channel-voice status.
TEST(MidiStreamParser, TruncatedSystemCommonClearedBySysEx) {
    StreamParser parser;
    auto events = FeedAll(parser, {0xF1, 0xF0, 0x7D, 0xF7, 0x90, 0x40, 0x50});

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 0x40);
    EXPECT_EQ(events[0].data2, 0x50);
}

// A complete System Common still consumes exactly its own data bytes, so the
// fix above must not make the parser under-consume and misread them as notes.
TEST(MidiStreamParser, CompleteSystemCommonStillConsumesItsData) {
    StreamParser parser;
    // F2 with both data bytes, then a note. Only the note should emerge.
    auto events = FeedAll(parser, {0xF2, 0x10, 0x20, 0x90, 0x3C, 0x64});

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 0x3C);
    EXPECT_EQ(events[0].data2, 0x64);
}

// Tune Request (0xF6) carries no data bytes: it must cancel running status
// (it is a System Common) without owing the stream anything, so the very
// next status byte parses normally and post-F6 orphan data is discarded.
TEST(MidiStreamParser, TuneRequestCancelsRunningStatusWithoutConsumingData) {
    StreamParser p;
    // Running-status note, then F6, then orphan data (must be dropped, since
    // F6 cancelled the 0x90 running status), then a full note-on.
    auto events = FeedAll(p, {0x90, 60, 100, 0xF6, 61, 99, 0x90, 62, 98});
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].data1, 60);
    EXPECT_EQ(events[1].data1, 62);
    EXPECT_EQ(events[1].data2, 98);
}

// A status byte arriving mid-message (sender aborted the previous message)
// must discard the half-assembled data, not splice old data1 with new data.
TEST(MidiStreamParser, InterruptedMessageIsAbandoned) {
    StreamParser p;
    // 0x90 60 (one data byte short), then a fresh complete note-on.
    auto events = FeedAll(p, {0x90, 60, 0x90, 72, 88});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::NoteOn);
    EXPECT_EQ(events[0].data1, 72);
    EXPECT_EQ(events[0].data2, 88);
}

}  // namespace
