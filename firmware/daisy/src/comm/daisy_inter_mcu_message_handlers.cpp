#include "daisy_inter_mcu_message_handlers.h"

#include <string.h>

#include "audio/audio_engine.h"
#include "comm/log_ring.h"
#include "config/link_config.h"
#include "config/logging_config.h"
#include "config/uart_debug_config.h"
#include "daisy_filesystem.h"
#include "daisy_seed.h"
#include "diag_push.h"
#include "mcu_link.h"
#include "spi_protocol/protocol.h"

#include "storage/card_service.hpp"

#if WAVEX_SPI_LINK_ENABLED
#include "daisy_spi_link.h"
#endif

namespace WaveX {
namespace Comm {

using namespace WaveX::Protocol;

// Forward declarations for message handler functions
static void HandleSyncMessage(const uint8_t* payload, size_t payload_size);
static void HandleControlChangeMessage(const uint8_t* payload, size_t payload_size);
static void HandleNoteMessage(const uint8_t* payload, size_t payload_size);
static void HandleNoteOffMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleLoadMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleDataMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleControlMessage(const uint8_t* payload, size_t payload_size);
static void HandleDataRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleMeterPushMessage(const uint8_t* payload, size_t payload_size);
static void HandleHeartbeatMessage(const uint8_t* payload, size_t payload_size);
static void HandleDiagSubscribeMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleEditMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleMetaReqMessage(const uint8_t* payload, size_t payload_size);
static void HandleMixOpMessage(const uint8_t* payload, size_t payload_size);
static void HandleTrackOpMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleSelectMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleUnloadMessage(const uint8_t* payload, size_t payload_size);
static void HandleTrackBindingReqMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleMetaPageReqMessage(const uint8_t* payload, size_t payload_size);
static void HandleEnvelopeReqMessage(const uint8_t* payload, size_t payload_size);
static void HandleStatusRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleBrowseRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleBrowseResponseMessage(const uint8_t* payload, size_t payload_size);
static void HandleSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleStopRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleStatusMessage(const uint8_t* payload, size_t payload_size);
static void HandleSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size);
static void HandleAckMessage(const uint8_t* payload, size_t payload_size);
static void HandleCvCalSetMessage(const uint8_t* payload, size_t payload_size);
static void HandleCvCalGetMessage(const uint8_t* payload, size_t payload_size);
static void HandleCvTestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSeqTransportMessage(const uint8_t* payload, size_t payload_size);
static void HandleSeqFileOpMessage(const uint8_t* payload, size_t payload_size);
static void HandleSeqPatternRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSeqPatternOpMessage(const uint8_t* payload, size_t payload_size);
static void HandleMidiClockEventMessage(const uint8_t* payload, size_t payload_size);
static void HandleMidiCcMessage(const uint8_t* payload, size_t payload_size);
static void HandleInstrumentOpMessage(const uint8_t* payload, size_t payload_size);
static void HandleErrorMessage(const uint8_t* payload, size_t payload_size);

// Message dispatcher - transport agnostic (works with both SPI and UART)
void ProcessInterMcuMessage(uint8_t msg_type,
                            uint16_t sequence_number,
                            const uint8_t* payload,
                            size_t payload_size) {
    if (msg_type == MSG_SAMPLE_PLAYHEAD) {
#if WAVEX_AUDIO_ENGINE_ENABLED
        SamplePlayheadRequest request;
        if (payload && payload_size == sizeof(request)) {
            std::memcpy(&request, payload, sizeof(request));
            if (IsValidSamplePlayheadRequest(request))
                AudioEngine::OnSamplePlayheadRequest(request);
        }
#endif
        return;
    }
    if (msg_type == MSG_SEQ_SONG_OP) {
#if WAVEX_AUDIO_ENGINE_ENABLED
        SeqSongOpMessage request;
        if (payload && payload_size == sizeof(request)) {
            std::memcpy(&request, payload, sizeof(request));
            if (IsValidSeqSongOp(request))
                AudioEngine::OnSongOp(request);
        }
#endif
        return;
    }
    if (msg_type == MSG_SEQ_SLOT_PAGE) {
#if WAVEX_AUDIO_ENGINE_ENABLED
        SeqPatternRequestMessage request;
        if (payload && payload_size == sizeof(request)) {
            std::memcpy(&request, payload, sizeof(request));
            if (IsValidSeqPatternRequest(request))
                AudioEngine::OnSeqSlotPageRequest(request);
        }
#endif
        return;
    }
    if (msg_type == MSG_SEQ_SLOT_EDIT) {
#if WAVEX_AUDIO_ENGINE_ENABLED
        SeqSlotEditMessage request;
        if (!AudioEngine::ProjectBusy() && !AudioEngine::BankBusy() &&
            !Storage::CardService::Busy() && payload && payload_size == sizeof(request)) {
            std::memcpy(&request, payload, sizeof(request));
            if (IsValidSeqSlotEdit(request))
                AudioEngine::OnSeqSlotEdit(request);
        }
#endif
        return;
    }
    if (msg_type == MSG_SEQ_SLOT_OP) {
#if WAVEX_AUDIO_ENGINE_ENABLED
        SeqSlotOpMessage request;
        if (payload && payload_size == sizeof(request)) {
            std::memcpy(&request, payload, sizeof(request));
            if (IsValidSeqSlotOp(request))
                AudioEngine::OnPatternSlotOp(request);
        }
#endif
        return;
    }
    if (msg_type == MSG_BANK_OP) {
#if WAVEX_AUDIO_ENGINE_ENABLED
        BankOpMessage request;
        if (payload && payload_size == sizeof(request)) {
            std::memcpy(&request, payload, sizeof(request));
            if (IsValidBankOp(request))
                AudioEngine::OnBankOp(request);
        }
#endif
        return;
    }
    if (msg_type == MSG_PROJECT_OP) {
#if WAVEX_AUDIO_ENGINE_ENABLED
        ProjectOpMessage request;
        if (payload && payload_size == sizeof(request)) {
            std::memcpy(&request, payload, sizeof(request));
            if (IsValidProjectOp(request))
                AudioEngine::OnProjectOp(request);
        }
#endif
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    bool project_stop = false;
    if (msg_type == MSG_SEQ_TRANSPORT && payload && payload_size == sizeof(SeqTransportMessage)) {
        SeqTransportMessage transport;
        std::memcpy(&transport, payload, sizeof(transport));
        project_stop = transport.command == SEQ_TRANSPORT_STOP;
    }
    // Project/Bank ownership freezes edits, notes and competing SD operations.
    // Releases and readback remain available; polling cannot replay a job.
    if ((AudioEngine::ProjectBusy() || AudioEngine::BankBusy()) && !project_stop &&
        msg_type != MSG_HEARTBEAT && msg_type != MSG_STATUS_REQUEST && msg_type != MSG_NOTE_OFF &&
        msg_type != MSG_MIX_STATE_REQ && msg_type != MSG_TRACK_STATE_REQ &&
        msg_type != MSG_SEQ_PATTERN_SYNC && msg_type != MSG_TRACK_BINDING_REQ &&
        msg_type != MSG_MIDI_CLOCK_EVENT)
        return;
#endif
    if (msg_type == MSG_CARD_OP) {
        if (payload && payload_size == sizeof(CardOpMessage)) {
            CardOpMessage request;
            std::memcpy(&request, payload, sizeof(request));
            if (IsValidCardOp(request))
                Storage::CardService::Request(request);
        }
        return;
    }
    // Maintenance owns the card and audio stop boundary. Do not queue new
    // playback or storage operations behind an accepted format request.
    if (Storage::CardService::Busy() && msg_type != MSG_HEARTBEAT)
        return;
#if WAVEX_MCU_LINK_PACKET_DEBUG
    // Per-message tracing: compile-gated (review M5 - this ran unconditionally
    // for every frame, including 20 Hz meter pushes, over blocking USB-CDC).
    if (s_hw) {
        WaveX::Log::PrintLine(
            "DAISY: Processing message - msg_type=0x%02X, seq=%u, payload_size=%d bytes",
            msg_type,
            sequence_number,
            static_cast<int>(payload_size));
        if (payload && payload_size > 0) {
            char hex[3 * 8 + 1] = {0};
            int pos = 0;
            const size_t preview = payload_size < 8 ? payload_size : 8;
            for (size_t i = 0; i < preview; ++i) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", payload[i]);
            }
            WaveX::Log::PrintLine("DAISY: Payload bytes: %s", hex);
        }
    }
#else
    (void)sequence_number;
#endif

    switch (msg_type) {
        case MSG_SYNC:
            HandleSyncMessage(payload, payload_size);
            break;
        case MSG_CONTROL_CHANGE:
            HandleControlChangeMessage(payload, payload_size);
            break;
        case MSG_MIX_OP:
            HandleMixOpMessage(payload, payload_size);
            break;
        case MSG_MIX_STATE_REQ: {
            MixStateRequest request{};
            if (payload && payload_size == sizeof(request)) {
                std::memcpy(&request, payload, sizeof(request));
                WaveX::AudioEngine::OnMixStateRequest(request);
            }
        } break;
        case MSG_TRACK_STATE_REQ: {
            TrackStateRequest request{};
            if (payload && payload_size == sizeof(request)) {
                std::memcpy(&request, payload, sizeof(request));
                WaveX::AudioEngine::OnTrackStateRequest(request);
            }
        } break;
        case MSG_TRACK_OP:
            HandleTrackOpMessage(payload, payload_size);
            break;
        case MSG_NOTE_ON:
            HandleNoteMessage(payload, payload_size);
            break;
        case MSG_NOTE_OFF:
            HandleNoteOffMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_LOAD:
            HandleSampleLoadMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_DATA:
            HandleSampleDataMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_CTRL:
            HandleSampleControlMessage(payload, payload_size);
            break;
        case MSG_DATA_REQUEST:
            HandleDataRequestMessage(payload, payload_size);
            break;
        case MSG_METER_PUSH:
            HandleMeterPushMessage(payload, payload_size);
            break;
        case MSG_STATUS_REQUEST:
            HandleStatusRequestMessage(payload, payload_size);
            break;
        case MSG_HEARTBEAT:
            HandleHeartbeatMessage(payload, payload_size);
            break;
        case MSG_BROWSE_REQ:
            HandleBrowseRequestMessage(payload, payload_size);
            break;
        case MSG_BROWSE_RESP:
            HandleBrowseResponseMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_PLAY_REQ:
            HandleSamplePlayRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_STOP_REQ:
            HandleSampleStopRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_STATUS:
            HandleSampleStatusMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_EDIT_SET:
            HandleSampleEditMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_SELECT:
            HandleSampleSelectMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_AUDITION: {
            if (!payload || payload_size != sizeof(SampleAuditionMessage)) {
                break;
            }
            SampleAuditionMessage msg;
            std::memcpy(&msg, payload, sizeof(msg));
            if (!WaveX::AudioEngine::AuditionSample(msg.sample_id)) {
                ErrorMessage error;
                error.code = 1;
                detail::CopyWireString(error.msg, sizeof(error.msg), "Sample audition failed");
                WaveX::Comm::LinkSend(MSG_ERROR, &error, sizeof(error));
            }
            break;
        }
        case MSG_SAMPLE_UNLOAD:
            HandleSampleUnloadMessage(payload, payload_size);
            break;
        case MSG_TRACK_BINDING_REQ:
            HandleTrackBindingReqMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_META_REQ:
            HandleSampleMetaReqMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_META_PAGE_REQ:
            HandleSampleMetaPageReqMessage(payload, payload_size);
            break;
        case MSG_ENVELOPE_REQ:
            HandleEnvelopeReqMessage(payload, payload_size);
            break;
        case MSG_DIAG_SUBSCRIBE:
            HandleDiagSubscribeMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_PLAY_INDEX_REQ:
            HandleSamplePlayIndexRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_GET_PATH_REQ:
            HandleSampleGetPathRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_GET_PATH_RESP:
            HandleSampleGetPathResponseMessage(payload, payload_size);
            break;
        case MSG_ACK:
            HandleAckMessage(payload, payload_size);
            break;
        case MSG_CV_CAL_SET:
            HandleCvCalSetMessage(payload, payload_size);
            break;
        case MSG_CV_CAL_GET:
            HandleCvCalGetMessage(payload, payload_size);
            break;
        case MSG_CV_TEST:
            HandleCvTestMessage(payload, payload_size);
            break;
        case MSG_SEQ_TRANSPORT:
            HandleSeqTransportMessage(payload, payload_size);
            break;
        case MSG_SEQ_FILE_OP:
            HandleSeqFileOpMessage(payload, payload_size);
            break;
        case MSG_SEQ_PATTERN_SYNC:
            HandleSeqPatternRequestMessage(payload, payload_size);
            break;
        case MSG_SEQ_PATTERN_OP:
            HandleSeqPatternOpMessage(payload, payload_size);
            break;
        case MSG_MIDI_CLOCK_EVENT:
            HandleMidiClockEventMessage(payload, payload_size);
            break;
        case MSG_MIDI_CC:
            HandleMidiCcMessage(payload, payload_size);
            break;
        case MSG_INST_MOD_OP:
#if WAVEX_AUDIO_ENGINE_ENABLED
            if (payload && payload_size == sizeof(InstModOpMessage)) {
                InstModOpMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                WaveX::AudioEngine::OnModOp(msg);
            }
#endif
            break;
        case MSG_INST_EDIT_OP:
#if WAVEX_AUDIO_ENGINE_ENABLED
            if (payload && payload_size == sizeof(InstEditOpMessage)) {
                InstEditOpMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                WaveX::AudioEngine::OnEditOp(msg);
            }
#endif
            break;
        case MSG_INST_LFO_OP:
#if WAVEX_AUDIO_ENGINE_ENABLED
            if (payload && payload_size == sizeof(InstLfoOpMessage)) {
                InstLfoOpMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                WaveX::AudioEngine::OnLfoOp(msg);
            }
#endif
            break;
        case MSG_INST_OSC_OP:
#if WAVEX_AUDIO_ENGINE_ENABLED
            if (payload && payload_size == sizeof(InstOscOpMessage)) {
                InstOscOpMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                WaveX::AudioEngine::OnOscOp(msg);
            }
#endif
            break;
        case MSG_INST_KEY_MAP_OP:
#if WAVEX_AUDIO_ENGINE_ENABLED
            if (payload && payload_size == sizeof(InstKeyMapOpMessage)) {
                InstKeyMapOpMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                WaveX::AudioEngine::OnKeyMapOp(msg);
            }
#endif
            break;
        case MSG_INST_PAD_SOUND_OP:
#if WAVEX_AUDIO_ENGINE_ENABLED
            if (payload && payload_size == sizeof(InstPadSoundOpMessage)) {
                InstPadSoundOpMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                WaveX::AudioEngine::OnPadSoundOp(msg);
            }
#endif
            break;
        case MSG_INST_OP:
            HandleInstrumentOpMessage(payload, payload_size);
            break;
        case MSG_ERROR:
            HandleErrorMessage(payload, payload_size);
            break;
        default:
            if (s_hw) {
                WaveX::Log::PrintLine("DAISY: Unknown message type: 0x%02X", msg_type);
            }
            break;
    }
}

// ---- Individual handler implementations ----

static void HandleSyncMessage(const uint8_t*, size_t) {}

// NOTE_ON/NOTE_OFF/CONTROL_CHANGE/SAMPLE_CTRL were log-only stubs until
// 2026-07-05 - the engine side (SPSC note queue -> VoiceManager) existed
// and was host-tested, but no wire message ever reached it (code review
// C1). These four now dispatch like the SAMPLE_LOAD handler
// below always did; tests/unit/comm/message_dispatch_test.cpp pins every
// routed type to its observable engine call so a stub can't silently
// reappear.

static void HandleControlChangeMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::ControlChangeMessage)) {
        UART_LOGE("daisy_msg", "CONTROL_CHANGE payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    const auto* msg = reinterpret_cast<const WaveX::Protocol::ControlChangeMessage*>(payload);
    WaveX::AudioEngine::OnControlChange(*msg);
#endif
}

static void HandleNoteMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::NoteMessage)) {
        UART_LOGE("daisy_msg", "NOTE_ON payload too small (%d)", (int)payload_size);
        return;
    }
    const auto* msg = reinterpret_cast<const WaveX::Protocol::NoteMessage*>(payload);
    // A valid frame CRC does not validate MIDI values. In particular, the
    // engine's release-overflow bitmap has exactly 128 note bits per Track.
    if (msg->note > 127 || msg->velocity > 127 || (msg->channel & 0x70u) != 0) {
        return;
    }

    // Logged on arrival, unconditionally.
    //
    // A note that played and a note that never arrived both produced exactly
    // nothing here: this handler had no log of its own, the per-message
    // dispatch trace is compiled out (WAVEX_MCU_LINK_PACKET_DEBUG defaults to
    // 0), and OnNoteOn's drop message sits behind an s_hw guard. So silence on
    // the console distinguished nothing, and cost a bench session working that
    // out. An instrument has to be able to say whether it heard you.
    WaveX::Log::PrintLine("RX NOTE_ON note=%u vel=%u ch=%u",
                          (unsigned)msg->note,
                          (unsigned)msg->velocity,
                          (unsigned)msg->channel);
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::AudioEngine::OnNoteOn(*msg);
#else
    WaveX::Log::PrintLine("  -> ignored: WAVEX_AUDIO_ENGINE_ENABLED is 0");
#endif
}

static void HandleNoteOffMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::NoteMessage)) {
        UART_LOGE("daisy_msg", "NOTE_OFF payload too small (%d)", (int)payload_size);
        return;
    }
    const auto* msg = reinterpret_cast<const WaveX::Protocol::NoteMessage*>(payload);
    if (msg->note > 127 || msg->velocity > 127 || (msg->channel & 0x70u) != 0) {
        return;
    }
    WaveX::Log::PrintLine("RX NOTE_OFF note=%u ch=%u", (unsigned)msg->note, (unsigned)msg->channel);
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::AudioEngine::OnNoteOff(*msg);
#endif
}

static void HandleSampleLoadMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleLoadMessage)) {
        if (s_hw) {
            WaveX::Log::PrintLine(
                "DAISY: Invalid payload size for SampleLoadMessage: %d (expected %d)",
                (int)payload_size,
                (int)sizeof(WaveX::Protocol::SampleLoadMessage));
        }
        return;
    }

    // A CRC-valid fixed-size payload can still fill the path field without a
    // terminator. Copy at the dispatch boundary so every downstream C-string
    // consumer (logging, FatFS, prefix checks) sees a bounded path. memcpy also
    // avoids binding a packed struct reference to a byte-aligned payload.
    WaveX::Protocol::SampleLoadMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    msg.path[sizeof(msg.path) - 1] = '\0';
    WaveX::AudioEngine::OnSampleLoad(msg);
}

// MSG_SAMPLE_DATA (push sample bytes over the link) is not implemented:
// the engine-side receiver was unreachable dead code and was removed
// (review C2). Samples load from the Daisy's own SD card (MSG_SAMPLE_LOAD).
// The message id stays reserved in protocol.h.
static void HandleSampleDataMessage(const uint8_t* payload, size_t payload_size) {
    (void)payload;
    (void)payload_size;  // only read by UART_LOGW, which compiles away
    UART_LOGW("daisy_msg",
              "MSG_SAMPLE_DATA ignored (%d bytes) - not implemented (review C2)",
              (int)payload_size);
}

static void HandleSampleControlMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleCtrlMessage)) {
        UART_LOGE("daisy_msg", "SAMPLE_CTRL payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    // memcpy, not reinterpret_cast: SampleCtrlMessage carries a float, and
    // the payload pointer is only byte-aligned.
    WaveX::Protocol::SampleCtrlMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::AudioEngine::OnSampleCtrl(msg);
#endif
}

static void HandleEnvelopeReqMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(EnvelopeReqMessage)) {
        if (s_hw) {
            WaveX::Log::PrintLine("DAISY: EnvelopeReq invalid size %d (expected %d)",
                                  (int)payload_size,
                                  (int)sizeof(EnvelopeReqMessage));
        }
        return;
    }

    EnvelopeReqMessage req{};
    memcpy(&req, payload, sizeof(req));

#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::AudioEngine::OnEnvelopeReq(req);
#else
    if (s_hw) {
        WaveX::Log::PrintLine("DAISY: Audio engine disabled; cannot process envelope req");
    }
#endif
}

static void HandleDataRequestMessage(const uint8_t*, size_t) {}

static void HandleMeterPushMessage(const uint8_t*, size_t) {}

static void HandleHeartbeatMessage(const uint8_t*, size_t) {}

static void HandleSampleSelectMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleSelectMessage)) {
        UART_LOGE("daisy_msg", "SAMPLE_SELECT payload too small (%d)", (int)payload_size);
        return;
    }
    const auto* msg = reinterpret_cast<const WaveX::Protocol::SampleSelectMessage*>(payload);
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::AudioEngine::SelectSample(msg->sample_id, msg->slot);
#else
    (void)msg;
#endif
}

static void HandleSampleUnloadMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleUnloadMessage)) {
        UART_LOGE("daisy_msg", "SAMPLE_UNLOAD payload too small (%d)", (int)payload_size);
        return;
    }
    const auto* msg = reinterpret_cast<const WaveX::Protocol::SampleUnloadMessage*>(payload);
#if WAVEX_AUDIO_ENGINE_ENABLED
    if (WaveX::AudioEngine::UnloadSample(msg->sample_id)) {
        // Tell the frontend what is left, so its list cannot drift from RAM.
        //
        // The metadata push alone cannot do that. MSG_SAMPLE_META only ever
        // describes a sample that exists, so pushing the remainder is silent
        // about what was removed - and unloading the last sample pushes nothing
        // at all. The frontend's meta cache is add/update-only and would keep
        // the freed entry forever, which is exactly why Unload appeared to do
        // nothing while the backend really had freed the memory.
        //
        // The status message is what carries a count and the full resident set,
        // so it is the one that can express a deletion. Send it unsolicited
        // here rather than waiting for the frontend's next poll, so the row
        // disappears when the user presses the key instead of up to a second
        // later.
        WaveX::AudioEngine::PushAllSampleMeta(0);
        WaveX::Protocol::SampleMemStatusMessage status{};
        WaveX::AudioEngine::GetSampleMemStatus(status);
        WaveX::Comm::LinkSend(WaveX::Protocol::MSG_STATUS_RESPONSE, &status, sizeof(status));
    }
#else
    (void)msg;
#endif
}

static void HandleTrackBindingReqMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(TrackBindingReqMessage)) {
        UART_LOGE("daisy_msg", "TRACK_BINDING_REQ payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    const auto* msg = reinterpret_cast<const TrackBindingReqMessage*>(payload);
    WaveX::AudioEngine::PushTrackBinding(msg->track);
#endif
}

static void HandleSampleMetaPageReqMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(SampleMetaPageReqMessage)) {
        UART_LOGE("daisy_msg", "SAMPLE_META_PAGE_REQ payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    SampleMetaPageReqMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::AudioEngine::RequestSampleMetaPage(msg.first, msg.count);
#endif
}

static void HandleSampleEditMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(SampleEditMessage)) {
        return;
    }
    SampleEditMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::AudioEngine::SetEditParams(msg.sample_id,
                                      msg.loop_enabled != 0,
                                      msg.gain_db_x10,
                                      msg.start_frame,
                                      msg.end_frame,
                                      msg.loop_start,
                                      msg.loop_end,
                                      msg.fade_in_ms,
                                      msg.fade_out_ms);
}

static void HandleMixOpMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(MixOpMessage)) {
        return;
    }
    MixOpMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::AudioEngine::OnMixOp(msg);
}

static void HandleTrackOpMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(TrackOpMessage)) {
        return;
    }
    TrackOpMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    if (IsValidTrackOp(msg))
        WaveX::AudioEngine::OnTrackOp(msg);
}

static void HandleSampleMetaReqMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(SampleMetaReqMessage)) {
        return;
    }
    SampleMetaReqMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    // 0 = every loaded sample, so the frontend can repopulate after its own
    // restart without the backend tracking who has seen what.
    WaveX::AudioEngine::PushAllSampleMeta(msg.sample_id);
}

static void HandleDiagSubscribeMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(DiagSubscribeMessage)) {
        return;
    }
    DiagSubscribeMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::Comm::DiagSubscribe(msg.enable != 0, msg.interval_hz);
}

static void HandleStatusRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::StatusRequestMessage)) {
        if (s_hw) {
            WaveX::Log::PrintLine("DAISY: Invalid status request payload (size=%d)",
                                  (int)payload_size);
        }
        return;
    }

    const auto* msg = reinterpret_cast<const WaveX::Protocol::StatusRequestMessage*>(payload);
    if (msg->category == STATUS_CATEGORY_SAMPLE_MEM) {
        WaveX::Protocol::SampleMemStatusMessage status{};
        WaveX::AudioEngine::GetSampleMemStatus(status);
        WaveX::Comm::LinkSend(WaveX::Protocol::MSG_STATUS_RESPONSE, &status, sizeof(status));
    }
}

// Browse request handler - transport agnostic (works via SPI or UART)
// Requires filesystem support and inter-MCU communication to be enabled
static void HandleBrowseRequestMessage(const uint8_t* payload, size_t payload_size) {
    uint8_t start_index = 0;
    char path[WaveX::Protocol::BROWSE_DIRECTORY_PATH_MAX]{};
    WaveX::Protocol::BrowseFilter filter{};
    if (!WaveX::Protocol::DecodeBrowseRequest(payload, payload_size, start_index, path, filter)) {
        UART_LOGE("daisy_uart", "BROWSE_REQ invalid path or filter");
        return;
    }
    WaveX::Comm::ProcessBrowseRequest(path, start_index, 20, filter);
}

static void HandleBrowseResponseMessage(const uint8_t*, size_t) {}

// Sample play request handler - requires inter-MCU comm and audio/filesystem support
static void HandleSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size == 0) {
        return;
    }
    // Same hazard as HandleBrowseRequestMessage above: a CRC-valid frame
    // carries no guarantee the path is NUL-terminated within payload_size,
    // and ProcessSamplePlayRequest takes a plain const char*. Bound the scan
    // and copy into a terminated buffer before handing it on.
    const char* path_ptr = reinterpret_cast<const char*>(payload);
    size_t path_len = strnlen(path_ptr, payload_size);
    char path[96] = {0};
    if (path_len >= sizeof(path)) {
        path_len = sizeof(path) - 1;
    }
    memcpy(path, path_ptr, path_len);
    path[path_len] = '\0';
    WaveX::Comm::ProcessSamplePlayRequest(path);
}

// Sample stop request handler - requires inter-MCU comm and audio support
static void HandleSampleStopRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleStopReqMessage)) {
        return;
    }
    const auto* msg = reinterpret_cast<const WaveX::Protocol::SampleStopReqMessage*>(payload);
    WaveX::Comm::ProcessSampleStopRequest(msg->slot);
}

static void HandleSampleStatusMessage(const uint8_t*, size_t) {}

// Sample play index request handler - requires inter-MCU comm and audio/filesystem support
static void HandleSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SamplePlayIndexMessage)) {
        if (s_hw) {
            WaveX::Log::PrintLine(
                "DAISY: Invalid payload size for SamplePlayIndexMessage: %d (expected %d)",
                (int)payload_size,
                (int)sizeof(WaveX::Protocol::SamplePlayIndexMessage));
        }
        return;
    }

    const auto* msg = reinterpret_cast<const WaveX::Protocol::SamplePlayIndexMessage*>(payload);

    if (s_hw) {
        WaveX::Log::PrintLine("DAISY: Parsed sample play index request - index=%lu",
                              static_cast<unsigned long>(msg->index));
    }

    // The gap belongs to this audition, not to the sample, so it is set here
    // rather than stored on the record.
    WaveX::AudioEngine::SetLoopGapMs(msg->loop_gap_ms);
    WaveX::Comm::ProcessSamplePlayIndexRequest(msg->index);
}

static void HandleSampleGetPathRequestMessage(const uint8_t*, size_t) {}

static void HandleSampleGetPathResponseMessage(const uint8_t*, size_t) {}

// CV calibration workflow (item 5 stage 4) - same validate-and-dispatch
// shape as the note handlers; pinned by message_dispatch_test.
static void HandleCvCalSetMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::CvCalMessage)) {
        UART_LOGE("daisy_msg", "CV_CAL_SET payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::Protocol::CvCalMessage msg;
    memcpy(&msg, payload, sizeof(msg));  // float fields: byte-aligned payload
    WaveX::AudioEngine::OnCvCalSet(msg);
#endif
}

static void HandleCvCalGetMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::CvCalGetMessage)) {
        UART_LOGE("daisy_msg", "CV_CAL_GET payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    const auto* msg = reinterpret_cast<const WaveX::Protocol::CvCalGetMessage*>(payload);
    WaveX::AudioEngine::OnCvCalGet(*msg);
#endif
}

static void HandleCvTestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::CvTestMessage)) {
        UART_LOGE("daisy_msg", "CV_TEST payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::Protocol::CvTestMessage msg;
    memcpy(&msg, payload, sizeof(msg));  // float fields: byte-aligned payload
    WaveX::AudioEngine::OnCvTest(msg);
#endif
}

// Sequencer / transport / MIDI-clock (Phase 2) - same validate-and-dispatch
// shape as the note/CV handlers; pinned by message_dispatch_test so a stub
// can't silently reappear (the C1 lesson). All forward to the engine-owned
// SequencerTransport via AudioEngine::On*.
static void HandleSeqTransportMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SeqTransportMessage)) {
        UART_LOGE("daisy_msg", "SEQ_TRANSPORT payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::Protocol::SeqTransportMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::AudioEngine::OnSeqTransport(msg);
#endif
}

static void HandleSeqFileOpMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size != sizeof(WaveX::Protocol::SeqFileOpMessage))
        return;
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::Protocol::SeqFileOpMessage request;
    memcpy(&request, payload, sizeof(request));
    WaveX::AudioEngine::OnSeqFileOp(request);
#endif
}

static void HandleSeqPatternRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SeqPatternRequestMessage))
        return;
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::Protocol::SeqPatternRequestMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::AudioEngine::OnSeqPatternRequest(msg);
#endif
}

static void HandleSeqPatternOpMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SeqPatternOpMessage)) {
        UART_LOGE("daisy_msg", "SEQ_PATTERN_OP payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::Protocol::SeqPatternOpMessage msg;
    memcpy(&msg, payload, sizeof(msg));  // int16 field: byte-aligned payload
    WaveX::AudioEngine::OnSeqPatternOp(msg);
#endif
}

static void HandleMidiClockEventMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::MidiClockEventMessage)) {
        UART_LOGE("daisy_msg", "MIDI_CLOCK_EVENT payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::Protocol::MidiClockEventMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::AudioEngine::OnMidiClockEvent(msg);
#endif
}

static void HandleMidiCcMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::MidiCcMessage)) {
        UART_LOGE("daisy_msg", "MIDI_CC payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    const auto* msg = reinterpret_cast<const WaveX::Protocol::MidiCcMessage*>(payload);
    WaveX::AudioEngine::OnMidiCc(*msg);
#endif
}

static void HandleInstrumentOpMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::InstOpMessage)) {
        UART_LOGE("daisy_msg", "INST_OP payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::Protocol::InstOpMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    msg.path[sizeof(msg.path) - 1] = '\0';
    WaveX::AudioEngine::OnInstrumentOp(msg);
#endif
}

static void HandleAckMessage(const uint8_t*, size_t) {}

static void HandleErrorMessage(const uint8_t*, size_t) {}

}  // namespace Comm
}  // namespace WaveX
