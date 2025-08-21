#include "message_router.h"
#include "daisy_seed.h"
#include "spi_protocol/protocol.h"
#include "inter_uart.h"
#include "../storage/fs_browse.h"
#include "../audio/audio_adapter.h"
#include "../metrics/metrics.h"
#include <cstring>

using namespace WaveX::Protocol;

// Extern Daisy log (provided by main for now)
extern daisy::DaisySeed hw;

void ProcessUARTMessage(const uint8_t* buffer, size_t length)
{
    if (!ProtocolHandler::ValidatePacket(buffer, length)) {
        #if WAVEX_UART_DEBUG_LOG
        hw.PrintLine("UART RX: invalid packet (len=%u)", (unsigned)length);
        #endif
        return;
    }

    MessageType msg_type = ProtocolHandler::GetMessageType(buffer);
    const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(buffer);
    const uint8_t payload_len = hdr ? hdr->length : 0;
    WaveX::Metrics::IncrementUartRxMessageCount();
    #if WAVEX_UART_DEBUG_LOG
    hw.PrintLine("UART RX: type=0x%02X len=%u (msg#=%lu)", (unsigned)msg_type, (unsigned)payload_len, (unsigned long)WaveX::Metrics::GetUartRxMessageCount());
    #endif

    switch (msg_type) {
        case MSG_BROWSE_REQ: {
            char path[WaveX::Protocol::BROWSE_PATH_MAX] = "/";
            uint32_t start_index = 0; uint8_t max_entries = 16;
            if(ProtocolHandler::ParseBrowseReq(buffer, path, sizeof(path), start_index, max_entries)) {
                WaveX::Storage::FileEntry tmp[12];
                std::memset(tmp, 0, sizeof(tmp));
                if(max_entries > 12) max_entries = 12; // keep payload small for now
                size_t total = 0;
                bool ok = WaveX::Storage::ListDir(path, tmp, max_entries, total, start_index);
                WaveX::Protocol::FileEntryWire wires[12];
                // Count how many entries were written by scanning until empty name or cap
                uint8_t n = 0;
                while(n < max_entries && tmp[n].name[0]) n++;
                uint8_t tx[256];
                // Cap by payload capacity (~256B total)
                const size_t tx_cap = (sizeof(tx) - sizeof(PacketHeader) - sizeof(BrowseRespHeader)) / sizeof(FileEntryWire);
                if(n > tx_cap) n = (uint8_t)tx_cap;
                for(uint8_t i = 0; i < n; i++) {
                    wires[i].is_dir = tmp[i].is_dir;
                    wires[i].size_bytes = tmp[i].size_bytes;
                    std::memset(wires[i].name, 0, sizeof(wires[i].name));
                    std::strncpy(wires[i].name, tmp[i].name, sizeof(wires[i].name) - 1);
                }
                size_t plen = ProtocolHandler::CreateBrowseRespPacket(tx, sizeof(tx), (uint32_t)total, wires, n);
                if(plen > 0) WaveX::Comm::Uart_Send(tx, plen);
            }
            break;
        }
        case MSG_SAMPLE_PLAY_REQ: {
            char path[WaveX::Protocol::BROWSE_PATH_MAX] = {0};
            if(ProtocolHandler::ParseSamplePlayReq(buffer, path, sizeof(path))) {
                // TODO: wire to SamplePlayer when implemented
                hw.PrintLine("SAMPLE_PLAY_REQ: %s", path);
            }
            break;
        }
        case MSG_SAMPLE_STOP_REQ: {
            // TODO: wire to SamplePlayer when implemented
            hw.PrintLine("SAMPLE_STOP_REQ");
            break;
        }
        case MSG_CONTROL_CHANGE: {
            ControlChangeMessage m;
            if (ProtocolHandler::ParseControlChange(buffer, m)) {
                WaveX::AudioAdapter::HandleControlChange(m);
            }
            break;
        }
        case MSG_NOTE_ON: {
            NoteMessage m;
            if (ProtocolHandler::ParseNoteMessage(buffer, m)) {
                WaveX::AudioAdapter::HandleNoteOn(m);
            }
            break;
        }
        case MSG_NOTE_OFF: {
            NoteMessage m;
            if (ProtocolHandler::ParseNoteMessage(buffer, m)) {
                WaveX::AudioAdapter::HandleNoteOff(m);
            }
            break;
        }
        case MSG_SAMPLE_CTRL: {
            SampleCtrlMessage m{};
            if(ProtocolHandler::ParseSampleCtrl(buffer, m)) {
                WaveX::AudioAdapter::HandleSampleCtrl(m);
            }
            break;
        }
        case MSG_PREVIEW_REQ: {
            PreviewReqMessage m{};
            if(ProtocolHandler::ParsePreviewReq(buffer, m)) {
                WaveX::AudioAdapter::HandlePreviewReq(m);
            }
            break;
        }
        case MSG_DATA_REQUEST: {
            DataRequestMessage dr{};
            if(ProtocolHandler::ParseDataRequest(buffer, dr)) {
                #if WAVEX_UART_DEBUG_LOG
                hw.PrintLine("RX DATA_REQUEST: type=%u", (unsigned)dr.request_type);
                #endif
                WaveX::Comm::QueuedMessage msg;
                if(WaveX::Comm::Uart_GetNextQueuedMessage(msg)) {
                    WaveX::Comm::Uart_PrepareResponsePacket(msg);
                } else {
                    uint8_t payload[] = {0};
                    uint8_t txbuf[64];
                    size_t packet_len = ProtocolHandler::CreateGenericPacket(
                        txbuf,
                        sizeof(txbuf),
                        MSG_SYNC,
                        payload,
                        0);
                    if(packet_len > 0) {
                        WaveX::Comm::Uart_Send(txbuf, packet_len);
                    }
                }
            }
            break;
        }
        case MSG_SYNC: {
            #if WAVEX_UART_DEBUG_LOG
            hw.PrintLine("RX SYNC");
            #endif
            break;
        }
        default:
            #if WAVEX_UART_DEBUG_LOG
            hw.PrintLine("RX UNKNOWN: type=0x%02X len=%u", (unsigned)msg_type, (unsigned)payload_len);
            #endif
            break;
    }
}


