#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;

TEST(BrowsePageProtocol, RequestAndReplyRoundTripRetainCorrelation) {
    BrowsePageRequest request;
    request.request_id = 0x12345678;
    request.start_index = 240;
    request.filter = BrowseFilter::Drum;
    std::strcpy(request.path, "/kits");
    std::array<uint8_t, 256> frame{};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  frame.data(), frame.size(), MSG_BROWSE_PAGE_REQ, &request, sizeof(request)),
              0u);
    BrowsePageRequest parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(frame.data(), MSG_BROWSE_PAGE_REQ, &parsed, sizeof(parsed)));
    EXPECT_TRUE(IsValidBrowsePageRequest(parsed));
    EXPECT_EQ(std::memcmp(&request, &parsed, sizeof(parsed)), 0);
    BrowsePageHeader page;
    page.request_id = request.request_id;
    page.start_index = request.start_index;
    page.filter = request.filter;
    BrowseRespHeader listing(241, 1);
    FileEntryWire entry(0, 12345, "kick.wav");
    std::array<uint8_t, sizeof(page) + sizeof(listing) + sizeof(entry)> payload{}, result{};
    std::memcpy(payload.data(), &page, sizeof(page));
    std::memcpy(payload.data() + sizeof(page), &listing, sizeof(listing));
    std::memcpy(payload.data() + sizeof(page) + sizeof(listing), &entry, sizeof(entry));
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  frame.data(), frame.size(), MSG_BROWSE_PAGE_RESP, payload.data(), payload.size()),
              0u);
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        frame.data(), MSG_BROWSE_PAGE_RESP, result.data(), result.size()));
    EXPECT_EQ(payload, result);
    EXPECT_TRUE(IsValidBrowsePageResponse(result.data(), result.size()));
    EXPECT_FALSE(IsValidBrowsePageResponse(result.data(), result.size() - 1));
    result[4] = 2;
    EXPECT_FALSE(IsValidBrowsePageResponse(result.data(), result.size()));
}
TEST(BrowsePageProtocol, RejectsInvalidIdentityVersionFilterAndPath) {
    BrowsePageRequest request;
    std::strcpy(request.path, "/");
    EXPECT_FALSE(IsValidBrowsePageRequest(request));
    request.request_id = 1;
    EXPECT_TRUE(IsValidBrowsePageRequest(request));
    request.version = 2;
    EXPECT_FALSE(IsValidBrowsePageRequest(request));
    request.version = 1;
    request.filter = static_cast<BrowseFilter>(255);
    EXPECT_FALSE(IsValidBrowsePageRequest(request));
    request.filter = BrowseFilter::All;
    std::memset(request.path, 'x', sizeof(request.path));
    EXPECT_FALSE(IsValidBrowsePageRequest(request));
}
