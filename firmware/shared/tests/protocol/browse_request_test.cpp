#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(BrowseRequestProtocol, LegacyAndFilteredRequestsRoundTripWithoutIndexChanges) {
    for (BrowseFilter filter:
         {BrowseFilter::All, BrowseFilter::Samples, BrowseFilter::Instruments}) {
        std::array<uint8_t, 128> encoded{}, packet{}, decoded{};
        const size_t bytes =
            EncodeBrowseRequest(encoded.data(), encoded.size(), "/kits", 200, filter);
        ASSERT_EQ(bytes, filter == BrowseFilter::All ? 7u : 8u);
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_BROWSE_REQ, encoded.data(), bytes),
                  0);
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_BROWSE_REQ, decoded.data(), bytes));
        char path[BROWSE_DIRECTORY_PATH_MAX]{};
        uint8_t index = 0;
        BrowseFilter mode = BrowseFilter::All;
        ASSERT_TRUE(DecodeBrowseRequest(decoded.data(), bytes, index, path, mode));
        EXPECT_STREQ(path, "/kits");
        EXPECT_EQ(index, 200);
        EXPECT_EQ(mode, filter);
    }
}
TEST(BrowseRequestProtocol, RejectsUnterminatedOversizedAndUnknownRequests) {
    std::array<uint8_t, 128> payload{};
    uint8_t index = 0;
    char path[BROWSE_DIRECTORY_PATH_MAX]{};
    BrowseFilter mode{};
    EXPECT_FALSE(DecodeBrowseRequest(nullptr, 0, index, path, mode));
    payload.fill('x');
    EXPECT_FALSE(DecodeBrowseRequest(payload.data(), payload.size(), index, path, mode));
    auto n = EncodeBrowseRequest(payload.data(), payload.size(), "/", 0, BrowseFilter::Instruments);
    payload[n - 1] = 99;
    EXPECT_FALSE(DecodeBrowseRequest(payload.data(), n, index, path, mode));
    payload[n - 1] = 2;
    EXPECT_FALSE(DecodeBrowseRequest(payload.data(), n + 1, index, path, mode));
    char long_path[BROWSE_DIRECTORY_PATH_MAX + 1];
    std::memset(long_path, 'x', sizeof(long_path));
    long_path[sizeof(long_path) - 1] = 0;
    EXPECT_EQ(EncodeBrowseRequest(payload.data(), payload.size(), long_path, 0), 0u);
    EXPECT_EQ(EncodeBrowseRequest(payload.data(), 1, "/", 0), 0u);
}
TEST(BrowseRequestProtocol, BrowserKindsClassifyOnlySupportedExtensions) {
    EXPECT_TRUE(BrowseFileMatches("Piano.WXI", BrowseFilter::Instruments));
    EXPECT_TRUE(BrowseFileMatches("Piano.SfZ", BrowseFilter::Instruments));
    EXPECT_FALSE(BrowseFileMatches("Piano.wav", BrowseFilter::Instruments));
    EXPECT_TRUE(BrowseFileMatches("Kick.WAV", BrowseFilter::Samples));
    EXPECT_FALSE(BrowseFileMatches("Piano.wxi", BrowseFilter::Samples));
    EXPECT_FALSE(BrowseFileMatches("Piano.wxi.tmp", BrowseFilter::All));
    EXPECT_FALSE(BrowseFileMatches("Piano", BrowseFilter::All));
    EXPECT_FALSE(BrowseFileMatches(nullptr, BrowseFilter::All));
}
