"""The harness itself: acks, sequence numbers, error paths, both boards."""

import pytest
from wavex_target import TargetError


@pytest.mark.esp32
def test_esp32_ping_and_unknown_verb(esp32):
    assert esp32.cmd("PING") == {}
    with pytest.raises(TargetError, match="ERR unknown"):
        esp32.cmd("NOSUCHVERB")


@pytest.mark.esp32
def test_esp32_state_has_the_navigator_basics(at_home):
    st = at_home.state()
    assert st["page"] == "Main_Menu"
    assert st["depth"] == "1"
    assert st["shift"] == "0"
    assert st["track"].isdigit()
    assert "dropped" in st


@pytest.mark.esp32
def test_esp32_bad_arguments_are_errors_not_silence(esp32):
    with pytest.raises(TargetError, match="badkey"):
        esp32.key("NOPE")
    with pytest.raises(TargetError, match="baddelta"):
        esp32.cmd("ENC", "x")
    with pytest.raises(TargetError, match="badxy"):
        esp32.cmd("TAP", 1)
    with pytest.raises(TargetError, match="badtrack"):
        esp32.track(99)


@pytest.mark.daisy
def test_daisy_ping_state_tracks_samples(daisy):
    assert daisy.cmd("PING") == {}
    st = daisy.state()
    for key in ("voices", "underruns", "samples", "streaming", "blocks"):
        assert st[key].isdigit(), st
    tracks = daisy.tracks()
    assert sorted(tracks) == list(range(16))
    assert isinstance(daisy.samples(), list)


@pytest.mark.daisy
def test_daisy_bad_msg_is_rejected(daisy):
    with pytest.raises(TargetError, match="badtype"):
        daisy.cmd("MSG", "zz")
    with pytest.raises(TargetError, match="badhex"):
        daisy.cmd("MSG", "02", "abc")
    with pytest.raises(TargetError, match="badnote"):
        daisy.cmd("NOTE", 99, 60, 100)


@pytest.mark.daisy
def test_daisy_malformed_note_payload_does_not_crash(daisy):
    # A NoteMessage is 4 bytes; hand the dispatcher 1 and 0 and expect the
    # board to still answer afterwards - the on-target fault injection the
    # design doc wanted.
    daisy.msg(0x02, b"\x3c")
    daisy.msg(0x02, b"")
    assert daisy.cmd("PING") == {}


@pytest.mark.daisy
def test_daisy_log_level_round_trip(daisy):
    daisy.cmd("LOG", "STORAGE", "DEBUG")
    daisy.cmd("LOG", "STORAGE", "INFO")
    with pytest.raises(TargetError, match="badlog"):
        daisy.cmd("LOG", "NOSUCHMODULE", "INFO")
