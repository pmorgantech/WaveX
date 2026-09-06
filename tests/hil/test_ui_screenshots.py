"""Capture the state of every ESP32 UI page and generate a screenshot report."""

import json
import os
import subprocess
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
SCREENSHOTS_DIR = os.path.join(REPO_ROOT, "docs", "ui_screenshots")


def _ensure_screenshots_dir():
    """Create screenshots directory if it doesn't exist."""
    os.makedirs(SCREENSHOTS_DIR, exist_ok=True)


def _capture_visual_screenshot(page_name, timeout_s=20.0):
    """Trigger a visual screenshot capture and save as PNG.

    Returns the path to the PNG file, or None if capture failed.
    """
    try:
        script = os.path.join(REPO_ROOT, "scripts", "esp32_screenshot.py")
        out_path = os.path.join(SCREENSHOTS_DIR, f"{page_name}.png")
        result = subprocess.run(
            ["python3", script, "--out", out_path, "--timeout", str(timeout_s)],
            capture_output=True,
            text=True,
            timeout=int(timeout_s) + 5,
        )
        if result.returncode == 0:
            return out_path
        else:
            # Don't print errors for expected timeouts on subsequent captures
            return None
    except Exception as e:
        return None


def _save_state(name, state):
    """Save page state to a JSON file."""
    _ensure_screenshots_dir()
    path = os.path.join(SCREENSHOTS_DIR, f"{name}.json")
    with open(path, "w") as f:
        json.dump(state, f, indent=2, sort_keys=True)
    return path


def _format_state_report(name, state):
    """Format state as a human-readable report."""
    report = f"# {name}\n\n"
    report += f"**Page:** {state.get('page', 'Unknown')}\n"
    report += f"**Depth:** {state.get('depth', 'N/A')}\n"
    report += f"**Track:** {state.get('track', 'N/A')}\n"

    if state.get('tab'):
        report += f"**Tab:** {state.get('tab', 'N/A')}\n"
        tabs = [state.get(f'tab{i}') for i in range(4)]
        tabs = [t for t in tabs if t]
        if tabs:
            report += f"**Available Tabs:** {', '.join(tabs)}\n"

    report += f"**Shift:** {state.get('shift', 'N/A')}\n"
    report += f"**Dropped Frames:** {state.get('dropped', 'N/A')}\n"

    # Softkeys
    softkey_count = 0
    softkeys_info = []
    for i in range(6):
        label = state.get(f"sk{i}")
        if label and label != "-":
            softkey_count += 1
            en = state.get(f"sk{i}en", "1") == "1"
            xy = state.get(f"sk{i}xy", "N/A")
            status = "✓" if en else "✗ (disabled)"
            softkeys_info.append(f"  {i}: {label.replace('_', ' ')} [{xy}] {status}")

    if softkeys_info:
        report += f"\n**Softkeys ({softkey_count}):**\n"
        report += "\n".join(softkeys_info)
        report += "\n"

    # Other state fields
    report += "\n**Full State:**\n```json\n"
    report += json.dumps(state, indent=2, sort_keys=True)
    report += "\n```\n"

    return report


@pytest.mark.esp32
def test_capture_all_ui_pages(at_home):
    """Navigate through every page and capture its state and visual screenshots."""
    esp = at_home

    # Dictionary to hold all captured states and screenshot paths
    all_pages = {}
    png_files = {}

    # Main menu - first screenshot
    st = esp.state()
    page_name = st.get('page', 'Unknown')
    all_pages[page_name] = st
    png_path = _capture_visual_screenshot(page_name)
    if png_path:
        png_files[page_name] = png_path
    print(f"\n📸 Captured: {page_name}" + (f" ✓ PNG" if png_path else ""))

    # Menu items to visit
    menu_items = ["Sample", "Play", "Instrument", "Settings", "Diagnostics"]

    for item in menu_items:
        try:
            # Navigate to the menu item
            st = esp.open_menu(item)
            page_name = st.get('page', 'Unknown')
            all_pages[page_name] = st
            png_path = _capture_visual_screenshot(page_name)
            if png_path:
                png_files[page_name] = png_path
            print(f"📸 Captured: {page_name}" + (f" ✓ PNG" if png_path else ""))

            # If this page has tabs, capture each tab
            if st.get('tab'):
                tabs = [st.get(f'tab{i}') for i in range(4)]
                tabs = [t for t in tabs if t and t != st.get('tab')]

                for tab_name in tabs:
                    try:
                        esp.page("TAB", tab_name)
                        tab_st = esp.wait_state(tab=tab_name, timeout=2.0)
                        page_tab_name = f"{page_name}_{tab_name}"
                        all_pages[page_tab_name] = tab_st
                        png_path = _capture_visual_screenshot(page_tab_name)
                        if png_path:
                            png_files[page_tab_name] = png_path
                        print(f"📸 Captured: {page_tab_name}" + (f" ✓ PNG" if png_path else ""))
                    except Exception as e:
                        print(f"⚠️  Could not capture tab {tab_name}: {e}")

            # Go back to menu
            esp.home()
            esp.wait_state(page="Main_Menu", depth="1")
        except Exception as e:
            print(f"⚠️  Could not capture {item}: {e}")
            esp.home()
            esp.wait_state(page="Main_Menu", depth="1")

    # Try Settings submenu items
    try:
        st = esp.open_menu("Settings")
        # Settings page might have sub-items accessible via encoder
        for i in range(3):
            try:
                esp.enc(1)
                st = esp.wait_state(timeout=1.0)
                page_name = st.get('page', 'Unknown')
                if page_name not in all_pages:
                    all_pages[page_name] = st
                    png_path = _capture_visual_screenshot(page_name)
                    if png_path:
                        png_files[page_name] = png_path
                    print(f"📸 Captured: {page_name}" + (f" ✓ PNG" if png_path else ""))
            except:
                break
        esp.home()
        esp.wait_state(page="Main_Menu", depth="1")
    except:
        pass

    # Save all pages
    _ensure_screenshots_dir()
    summary_path = os.path.join(SCREENSHOTS_DIR, "00_SUMMARY.md")

    with open(summary_path, "w") as f:
        f.write("# WaveX ESP32 UI Page Gallery\n\n")
        f.write(f"**Generated:** {time.strftime('%Y-%m-%d %H:%M:%S')}\n\n")
        f.write(f"**Total Pages Captured:** {len(all_pages)}\n")
        f.write(f"**Visual Screenshots:** {len(png_files)} of {len(all_pages)}\n\n")
        f.write("## Page List\n\n")

        for i, page_name in enumerate(sorted(all_pages.keys()), 1):
            status = "📸" if page_name in png_files else ""
            f.write(f"{i}. [{page_name}](#{page_name.replace(' ', '-').lower()}) {status}\n")

        f.write("\n---\n\n")

        for page_name in sorted(all_pages.keys()):
            state = all_pages[page_name]
            report = _format_state_report(page_name, state)

            # Add PNG screenshot if available
            if page_name in png_files:
                png_file = os.path.basename(png_files[page_name])
                report = f"![{page_name} Screenshot]({png_file})\n\n" + report

            f.write(report)
            f.write("\n---\n\n")

            # Also save individual JSON file
            _save_state(page_name.replace(" ", "_"), state)

    print(f"\n✅ Captured {len(all_pages)} pages")
    print(f"📸 Visual screenshots: {len(png_files)}/{len(all_pages)}")
    print(f"📁 Screenshots saved to: {SCREENSHOTS_DIR}")
    print(f"📄 Summary: {summary_path}")
    if png_files:
        print(f"\n📸 PNG files:")
        for page_name in sorted(png_files.keys()):
            print(f"  - {os.path.basename(png_files[page_name])}")

    # Print paths for user
    assert len(all_pages) > 0, "Failed to capture any pages"
