"""The panel's keys, injected by name: jumps, softkeys, Shift, Track -/+.

Every key goes through the console's KEY verb exactly as the keypad task
posts a matrix key, so what is exercised is the InputDispatcher's key
semantics (panel-controls.md §4.3), not the matrix itself. The matrix is
verified on the Diagnostics ▸ Panel tab by hand.
"""

import pytest


@pytest.mark.esp32
def test_jump_keys_reach_each_root_group(at_home):
    esp = at_home
    st = esp.state()
    assert st["root"] == "-", st
    for group, page in (
        ("Sample", "Sample"),
        ("Play", "Play"),
        ("Instrument", "Instrument"),
        ("Settings", "Settings"),
    ):
        st = esp.jump(group)
        assert st["page"] == page, st
        assert st["lastkey"] == group.upper(), st
    esp.home()
    esp.wait_state(page="Main_Menu", depth="1", root="-")


@pytest.mark.esp32
def test_jump_from_a_deep_page_unwinds_to_the_new_root(at_home):
    esp = at_home
    esp.jump("Sample")
    esp.page("TAB", "Browse")
    esp.wait_state(tab="Browse")
    # Wherever the user is, a jump lands on the group's page at depth 2 -
    # nothing of the old stack survives.
    st = esp.jump("Play")
    assert st["page"] == "Play", st
    esp.key("BACK")
    esp.wait_state(page="Main_Menu", depth="1", root="-")


@pytest.mark.esp32
def test_jump_to_a_group_without_a_page_is_refused(at_home):
    esp = at_home
    esp.jump("Sample")
    # Track and Mixer have keys before they have pages (Phase 2.5); the key
    # is seen but the stack does not move.
    esp.key("MIXER")
    st = esp.wait_state(lastkey="MIXER")
    assert st["page"] == "Sample" and st["root"] == "Sample", st


@pytest.mark.esp32
def test_softkey_by_panel_key_fires_the_labelled_action(at_home):
    esp = at_home
    st = esp.jump("Sample")
    idx, enabled, _ = esp.softkeys(st)["Back"]
    assert enabled
    esp.key(f"SOFT{idx + 1}")
    esp.wait_state(page="Main_Menu", depth="1")


@pytest.mark.esp32
def test_empty_softkey_slot_does_nothing(at_home):
    esp = at_home
    esp.jump("Sample")
    esp.key("SHIFT")
    st = esp.wait_state(shift="1")
    used = {i for i, _, _ in esp.softkeys(st).values()}
    free = next(i for i in range(6) if i not in used)
    esp.key(f"SOFT{free + 1}")
    st = esp.wait_state(lastkey=f"SOFT{free + 1}")
    # No action ran, so Shift was not consumed either.
    assert st["page"] == "Sample" and st["shift"] == "1", st
    esp.key("SHIFT")
    esp.wait_state(shift="0")


@pytest.mark.esp32
def test_shift_then_softkey_fires_the_shifted_row_once(at_home):
    esp = at_home
    esp.track(0)
    esp.jump("Sample")  # Sample Manager: shifted row carries Track -/+
    esp.key("SHIFT")
    st = esp.wait_state(shift="1")
    idx, enabled, _ = esp.softkeys(st)["Track +"]
    assert enabled
    esp.key(f"SOFT{idx + 1}")
    # The shifted action ran and Shift unstuck itself.
    st = esp.wait_state(track="1", shift="0")
    esp.track(0)


@pytest.mark.esp32
def test_track_keys_step_and_wrap(at_home):
    esp = at_home
    esp.track(0)
    esp.jump("Play")
    esp.key("TRACK_NEXT")
    esp.wait_state(track="1")
    esp.key("TRACK_PREV")
    esp.wait_state(track="0")
    esp.key("TRACK_PREV")
    esp.wait_state(track="15")
    esp.track(0)


@pytest.mark.esp32
def test_transport_and_pad_keys_are_seen_but_inert(at_home):
    esp = at_home
    st = esp.jump("Sample")
    for name in ("PLAY_STOP", "REC", "PAD1", "PAD16"):
        esp.key(name)
        st = esp.wait_state(lastkey=name)
        assert st["page"] == "Sample" and st["depth"] == "2", st


@pytest.mark.esp32
def test_unknown_key_name_is_refused(at_home):
    esp = at_home
    from wavex_target import TargetError

    with pytest.raises(TargetError):
        esp.key("SOFT7")
