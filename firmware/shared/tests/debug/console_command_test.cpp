// The console grammar is shared by both boards precisely so it can be pinned
// here once; these tests are the contract scripts/ and tests/hil/ rely on.
#include "debug/console_command.h"

#include <gtest/gtest.h>

#include <string>

using namespace WaveX::Debug;

namespace {

void FeedAll(LineReader& r, const std::string& s) {
    for (char c: s) {
        r.Feed(c);
    }
}

}  // namespace

TEST(ConsoleLineReader, CompletesOnNewlineAndHoldsUntilReleased) {
    LineReader r;
    EXPECT_FALSE(r.Ready());
    FeedAll(r, "WAVEX-DBG 1 STATE");
    EXPECT_FALSE(r.Ready());
    EXPECT_TRUE(r.Feed('\n'));
    ASSERT_TRUE(r.Ready());
    EXPECT_STREQ(r.Line(), "WAVEX-DBG 1 STATE");

    // Bytes arriving while the line is held are dropped and counted, never
    // written over the line being parsed.
    FeedAll(r, "WAVEX-DBG 2 STATE\n");
    EXPECT_STREQ(r.Line(), "WAVEX-DBG 1 STATE");
    EXPECT_EQ(r.DroppedBytes(), 18u);

    r.Release();
    EXPECT_FALSE(r.Ready());
    FeedAll(r, "WAVEX-DBG 3 KEY SELECT TAP\r\n");
    ASSERT_TRUE(r.Ready());
    EXPECT_STREQ(r.Line(), "WAVEX-DBG 3 KEY SELECT TAP");
}

TEST(ConsoleLineReader, IgnoresEmptyLinesAndCrLfPairs) {
    LineReader r;
    FeedAll(r, "\r\n\n\r");
    EXPECT_FALSE(r.Ready());
    FeedAll(r, "x\r\n");
    ASSERT_TRUE(r.Ready());
    EXPECT_STREQ(r.Line(), "x");
    r.Release();
    // The '\n' of a CRLF pair after Release must not produce an empty line.
    EXPECT_FALSE(r.Ready());
}

TEST(ConsoleLineReader, OverLongLineIsRejectedWholeNotTruncated) {
    LineReader r;
    std::string huge = "WAVEX-DBG 4 MSG ";
    huge += std::string(kMaxLineBytes, 'A');
    FeedAll(r, huge);
    EXPECT_FALSE(r.Ready());
    EXPECT_FALSE(r.Feed('\n'));
    EXPECT_FALSE(r.Ready()) << "a cut-off command must never surface as a shorter one";
    EXPECT_EQ(r.OverflowedLines(), 1u);

    // And the reader recovers for the next line.
    FeedAll(r, "WAVEX-DBG 5 STATE\n");
    ASSERT_TRUE(r.Ready());
    EXPECT_STREQ(r.Line(), "WAVEX-DBG 5 STATE");
}

TEST(ConsoleParse, DbgLineWithSeqVerbAndArgs) {
    Command c;
    ASSERT_TRUE(ParseCommand("WAVEX-DBG 42 key select tap", c));
    EXPECT_EQ(c.seq, 42);
    EXPECT_STREQ(c.verb, "KEY");
    EXPECT_STREQ(c.args, "select tap");
    EXPECT_FALSE(c.legacy);
}

TEST(ConsoleParse, DbgLineWithoutArgs) {
    Command c;
    ASSERT_TRUE(ParseCommand("  WAVEX-DBG 7 STATE  ", c));
    EXPECT_EQ(c.seq, 7);
    EXPECT_STREQ(c.verb, "STATE");
    EXPECT_STREQ(c.args, "");
}

TEST(ConsoleParse, DbgLineMissingSeqIsMalformedNotIgnored) {
    Command c;
    ASSERT_TRUE(ParseCommand("WAVEX-DBG STATE", c));
    EXPECT_EQ(c.seq, kNoSeq);
    EXPECT_STREQ(c.verb, "") << "empty verb signals 'answer ERR', not 'stay silent'";
}

TEST(ConsoleParse, LegacyLinesKeepTheirVerbAndNoSeq) {
    Command c;
    ASSERT_TRUE(ParseCommand("WAVEX-LOG STORAGE DEBUG", c));
    EXPECT_TRUE(c.legacy);
    EXPECT_EQ(c.seq, kNoSeq);
    EXPECT_STREQ(c.verb, "LOG");
    EXPECT_STREQ(c.args, "STORAGE DEBUG");

    ASSERT_TRUE(ParseCommand("WAVEX-FILTER daisysp", c));
    EXPECT_STREQ(c.verb, "FILTER");
    EXPECT_STREQ(c.args, "daisysp");

    ASSERT_TRUE(ParseCommand("WAVEX-ENTER-DFU", c));
    EXPECT_STREQ(c.verb, "ENTER-DFU");
}

TEST(ConsoleParse, LeadingJunkBeforeTheMarkerIsIgnored) {
    // The first line after a port opens can start with bytes already in
    // flight; the command must still be found.
    Command c;
    ASSERT_TRUE(ParseCommand("\x7f\x7fgarbage WAVEX-DBG 10 PING", c));
    EXPECT_EQ(c.seq, 10);
    EXPECT_STREQ(c.verb, "PING");
}

TEST(ConsoleParse, OrdinaryTrafficIsNotACommand) {
    Command c;
    c.seq = 99;
    EXPECT_FALSE(ParseCommand("I (1234) UI_NAVIGATOR: pushed Play", c));
    EXPECT_FALSE(ParseCommand("", c));
    EXPECT_FALSE(ParseCommand(nullptr, c));
    EXPECT_EQ(c.seq, 99) << "a non-command must leave the struct untouched";
}

TEST(ConsoleReply, OkAndErrLinesCarryTheSeq) {
    char out[128];
    size_t n = FormatOk(12, out, sizeof(out));
    EXPECT_STREQ(out, "WAVEX-DBG: 12 OK");
    n = AppendKv(out, sizeof(out), n, "page", "Play");
    n = AppendKvInt(out, sizeof(out), n, "depth", 2);
    n = AppendKvText(out, sizeof(out), n, "status", "Load into Track 3?");
    EXPECT_STREQ(out, "WAVEX-DBG: 12 OK page=Play depth=2 status=Load_into_Track_3?");
    EXPECT_EQ(n, std::strlen(out));

    FormatErr(13, "busy", out, sizeof(out));
    EXPECT_STREQ(out, "WAVEX-DBG: 13 ERR busy");
}

TEST(ConsoleReply, AppendStopsCleanlyAtCapacity) {
    char out[32];
    size_t n = FormatOk(1, out, sizeof(out));
    for (int i = 0; i < 20; ++i) {
        n = AppendKv(out, sizeof(out), n, "k", "vvvvvvvv");
    }
    EXPECT_LT(n, sizeof(out));
    EXPECT_EQ(std::strlen(out), n);
}

TEST(ConsoleHex, ParsesEvenLengthHexAndRejectsTheRest) {
    uint8_t buf[8];
    EXPECT_EQ(ParseHexBytes("00ff7A", buf, sizeof(buf)), 3u);
    EXPECT_EQ(buf[0], 0x00);
    EXPECT_EQ(buf[1], 0xFF);
    EXPECT_EQ(buf[2], 0x7A);
    EXPECT_EQ(ParseHexBytes("abc", buf, sizeof(buf)), 0u) << "odd length";
    EXPECT_EQ(ParseHexBytes("zz", buf, sizeof(buf)), 0u) << "not hex";
    EXPECT_EQ(ParseHexBytes("0011223344556677889900", buf, sizeof(buf)), 0u) << "overflow";
    EXPECT_EQ(ParseHexBytes("", buf, sizeof(buf)), 0u);
}

TEST(ConsoleTokens, NextIntAndNextWordWalkTheArgs) {
    const char* p = "select TAP -3 +4 x";
    char w[16];
    long v;
    ASSERT_TRUE(NextWord(&p, w, sizeof(w)));
    EXPECT_STREQ(w, "SELECT");
    ASSERT_TRUE(NextWord(&p, w, sizeof(w)));
    EXPECT_STREQ(w, "TAP");
    ASSERT_TRUE(NextInt(&p, &v));
    EXPECT_EQ(v, -3);
    ASSERT_TRUE(NextInt(&p, &v));
    EXPECT_EQ(v, 4);
    EXPECT_FALSE(NextInt(&p, &v)) << "'x' is not an integer";
    ASSERT_TRUE(NextWord(&p, w, sizeof(w)));
    EXPECT_STREQ(w, "X");
    EXPECT_FALSE(NextWord(&p, w, sizeof(w)));
}
