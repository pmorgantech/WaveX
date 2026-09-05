"""Page routing through injected input: menu, tabs, softkeys, Shift."""

import pytest


@pytest.mark.esp32
def test_menu_encoder_and_select_reach_each_group(at_home):
    esp = at_home
    for item, page in (
        ("Sample", "Sample"),
        ("Play", "Play"),
        ("Instrument", "Instrument"),
    ):
        st = esp.open_menu(item)
        assert st["page"] == page, st
        assert st["depth"] == "2"
        esp.home()
        esp.wait_state(page="Main_Menu", depth="1")


@pytest.mark.esp32
def test_back_key_pops_a_page(at_home):
    esp = at_home
    esp.open_menu("Play")
    esp.key("BACK")
    esp.wait_state(page="Main_Menu", depth="1")


@pytest.mark.esp32
def test_softkeys_are_reported_with_geometry_and_tappable(at_home):
    esp = at_home
    st = esp.open_menu("Sample")
    keys = esp.softkeys(st)
    assert "Back" in keys, keys
    idx, enabled, pt = keys["Back"]
    assert enabled and pt and pt[0] > 0 and pt[1] > 0
    # Tapping the on-screen Back through the synthetic touch indev pops the
    # page exactly as a finger would.
    esp.softkey("Back", st)
    esp.wait_state(page="Main_Menu", depth="1")


@pytest.mark.esp32
def test_tab_host_reports_and_switches_tabs(at_home):
    esp = at_home
    st = esp.open_menu("Sample")
    assert st["tab"] == "Manage", st
    titles = [st.get(f"tab{i}") for i in range(4)]
    assert titles == ["Manage", "Browse", "Edit", "Record"], titles
    esp.page("TAB", "Browse")
    st = esp.wait_state(tab="Browse")
    # And through the tab bar itself, by touch.
    x, y = (int(v) for v in st["tab2xy"].split(","))
    esp.tap(x, y)
    esp.wait_state(tab="Edit")


@pytest.mark.esp32
def test_shift_is_a_sticky_modifier(at_home):
    esp = at_home
    esp.open_menu("Sample")  # Sample Manager has a shifted row (Track -/+)
    esp.key("SHIFT")
    st = esp.wait_state(shift="1")
    assert "Track -" in esp.softkeys(st), st
    esp.key("SHIFT")
    esp.wait_state(shift="0")


@pytest.mark.esp32
def test_track_verb_selects_and_reports(at_home):
    esp = at_home
    st = esp.track(3)
    assert st["track"] == "3"
    st = esp.wait_state(track="3", tstate=lambda s: s != "unknown")
    assert st["tstate"] in ("empty", "sample", "instrument", "loading")
    esp.track(0)
