"""tests/hil fixtures: board discovery and session-scoped targets.

Runs from the devcontainer with both boards attached and the serial loggers
up (`make test-hil` does the checks). With no board enumerated every test
skips - a laptop run is a no-op, not a failure.

Markers: esp32, daisy, both, sdcard, slow.
"""

import os
import sys
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from wavex_target import Daisy, Esp32, TargetError  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(HERE))


def pytest_configure(config):
    for m in ("esp32", "daisy", "both", "sdcard", "slow"):
        config.addinivalue_line(
            "markers",
            f"{m}: needs the {m} target / resource",
        )


def pytest_addoption(parser):
    parser.addoption(
        "--hil-slow",
        action="store_true",
        help="run @slow soak tests too",
    )
    parser.addoption(
        "--hil-sample",
        default=os.environ.get(
            "WAVEX_HIL_SAMPLE",
            "/Drums/Kicks/bassdr01.wav",
        ),
        help="card path of a short PCM16 WAV the Load tests use",
    )
    parser.addoption(
        "--hil-sample2",
        default=os.environ.get(
            "WAVEX_HIL_SAMPLE2",
            "/Drums/Kicks/bassdr02.wav",
        ),
        help="a second short PCM16 WAV in the same directory",
    )


def pytest_collection_modifyitems(config, items):
    if config.getoption("--hil-slow"):
        return
    skip = pytest.mark.skip(reason="soak test; pass --hil-slow")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip)


@pytest.fixture(scope="session")
def run_log():
    os.makedirs(os.path.join(REPO_ROOT, "logs"), exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(REPO_ROOT, "logs", f"hil-{stamp}.log")
    with open(path, "w") as fh:
        yield fh


def _target(cls, run_log):
    try:
        t = cls(run_log=run_log)
    except TargetError as exc:
        pytest.skip(str(exc))
    if not t.probe():
        pytest.skip(
            f"{t.board}: console did not answer "
            "(image without the harness, or the logger is stale)"
        )
    return t


@pytest.fixture(scope="session")
def esp32(run_log):
    t = _target(Esp32, run_log)
    yield t
    t.close()


@pytest.fixture(scope="session")
def daisy(run_log):
    t = _target(Daisy, run_log)
    yield t
    t.close()


@pytest.fixture
def sample_path(request):
    return request.config.getoption("--hil-sample")


@pytest.fixture
def sample_path2(request):
    return request.config.getoption("--hil-sample2")


@pytest.fixture
def at_home(esp32):
    """Every UI test starts and ends on the main menu."""
    esp32.home()
    esp32.wait_state(page="Main_Menu")
    yield esp32
    esp32.home()
