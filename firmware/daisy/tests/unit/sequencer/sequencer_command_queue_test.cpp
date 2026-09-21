#include "sequencer/sequencer_command_queue.hpp"

#include <gtest/gtest.h>

using WaveX::Protocol::SEQ_CLOCK_INTERNAL;
using WaveX::Protocol::SEQ_INPUT_PLAY;
using WaveX::Protocol::SEQ_TRANSPORT_PLAY;
using WaveX::Protocol::SeqTransportMessage;
using WaveX::Sequencer::SequencerCommand;
using WaveX::Sequencer::SequencerCommandQueue;
using WaveX::Sequencer::SequencerCommandType;

TEST(SequencerCommandQueueTest, PreservesCommandOrderAndPayload) {
    SequencerCommandQueue<2> queue;
    queue.Init();

    SequencerCommand first;
    first.type = SequencerCommandType::Transport;
    first.transport =
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12345, 0);
    SequencerCommand second;
    second.type = SequencerCommandType::MidiClock;
    second.midi_clock = WaveX::Protocol::MidiClockEventMessage(0, 1, 99, 100, 3);

    ASSERT_TRUE(queue.Push(first));
    ASSERT_TRUE(queue.Push(second));

    SequencerCommand received;
    ASSERT_TRUE(queue.Pop(received));
    EXPECT_EQ(received.type, SequencerCommandType::Transport);
    EXPECT_EQ(received.transport.tempo_bpm_x100, 12345);
    ASSERT_TRUE(queue.Pop(received));
    EXPECT_EQ(received.type, SequencerCommandType::MidiClock);
    EXPECT_EQ(received.midi_clock.event, 0);
    EXPECT_EQ(received.midi_clock.tick_seq, 99u);
    EXPECT_EQ(received.midi_clock.spp_beats16, 3);
    EXPECT_FALSE(queue.Pop(received));
}

TEST(SequencerCommandQueueTest, RejectsNewCommandWhenFullWithoutOverwritingOldest) {
    SequencerCommandQueue<1> queue;
    queue.Init();

    SequencerCommand command;
    command.type = SequencerCommandType::MidiClock;
    command.midi_clock = WaveX::Protocol::MidiClockEventMessage(1, 0, 2, 3, 0);
    ASSERT_TRUE(queue.Push(command));
    EXPECT_FALSE(queue.Push(command));

    SequencerCommand received;
    ASSERT_TRUE(queue.Pop(received));
    EXPECT_EQ(received.midi_clock.event, 1);
}
