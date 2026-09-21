"""Session LFO readback and held-step lock lifetime on matched boards."""

import pytest
from test_pattern_files import _back, _files, _new


@pytest.mark.both
def test_global_lfo_settings_survive_page_reentry(at_home):
    esp = at_home
    esp.open_menu("Settings")
    esp.page("TAB", "Global LFO")
    state = esp.wait_state(globalready=1)
    for field, key, value in [
        (0, "globalwave", 2),
        (1, "globalrate", 250),
        (2, "globalsync", 3),
        (3, "globalrestart", 1),
    ]:
        esp.page("ADJUST", field, value - int(state[key]))
        state = esp.wait_state(globalready=1, **{key: value})
    esp.softkey("Reset phase")
    esp.wait_state(globalready=1)
    esp.page("TAB", "Display")
    esp.page("TAB", "Global LFO")
    esp.wait_state(
        globalready=1,
        globalwave=2,
        globalrate=250,
        globalsync=3,
        globalrestart=1,
    )
    for field, key, value in [
        (0, "globalwave", 0),
        (1, "globalrate", 100),
        (2, "globalsync", 0),
        (3, "globalrestart", 0),
    ]:
        state = esp.state()
        esp.page("ADJUST", field, value - int(state[key]))
        esp.wait_state(globalready=1, **{key: value})


@pytest.mark.both
def test_held_step_writes_lock_without_toggling_note(at_home):
    esp = at_home
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    _files(esp)
    _new(esp)
    _back(esp)
    esp.page("FOCUS", 1, 1)
    esp.wait_state(seqready=1, seqbits=0)
    esp.page("HOLD", 1)
    esp.wait_state(holding=1, seqlocks=1)
    esp.page("HELDLOCK", 0, -4)
    esp.wait_state(seqready=1, holding=1, lockparam=2, lockvalue=64511)
    esp.page("HOLD", 0)
    esp.wait_state(holding=0)
    esp.softkey("Grid")
    esp.wait_state(seqlocks=0, seqbits=0)
    esp.page("TOGGLE")
    esp.wait_state(seqready=1, seqbits=1)
    esp.page("HOLD", 1)
    esp.wait_state(holding=1)
    esp.track(1)
    esp.wait_state(holding=0)


@pytest.mark.both
def test_diagnostics_count_received_midi_not_track_notes(at_home, daisy):
    esp = at_home
    esp.open_menu("Diagnostics")
    esp.wait_state(midinotes=0, midiccs=0)
    # Track-addressed UI notes are deliberately excluded.
    daisy.note(15, 60, 100, True)
    daisy.note(15, 60, 0, False)
    esp.wait_state(midinotes=0)
    # Console injection follows received MIDI dispatch.
    # Physical ports are covered by HV-014.
    daisy.midi_note(16, 60, 100, True)
    esp.wait_state(midinotes=lambda n: int(n) > 0)
    daisy.midi_note(16, 60, 0, False)
    esp.cmd("MIDICC", 16, 1, 40)
    esp.wait_state(midiccs=lambda n: int(n) > 0)
    esp.wait_state(midinotes=0, midiccs=0)
