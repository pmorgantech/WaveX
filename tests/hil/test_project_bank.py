"""HV-016j: Project Bank selection round trip with no sample prerequisites.

Creates unique empty Banks/Projects and leaves the files for reboot checks.
The existing bench session is replaced; no saved files are overwritten.
"""

import time

import pytest
from test_bank_files import _banks, _operation
from test_project_files import empty_project  # noqa: F401
from test_project_files import _confirmed, _files


@pytest.mark.both
@pytest.mark.sdcard
@pytest.mark.usefixtures("empty_project")
def test_project_restores_bank_selection(esp32, daisy, record_property):
    esp = esp32
    prefix = "HILPB " + str(time.time_ns())[-13:]
    first, second, empty = (prefix + suffix for suffix in "ABC")
    record_property("saved_bank_project", first)
    record_property("updated_bank_project", second)
    record_property("empty_bank_project", empty)

    def save(name):
        _files(esp)
        esp.page("NAME", name)
        esp.softkey("Save copy")
        esp.wait_state(
            projectready=1,
            projectpending=0,
            projectbusy=0,
            projecterror=0,
        )

    def load(name):
        _files(esp)
        esp.page("NAME", name)
        _confirmed(esp, "Load", 2)

    def bank_name(name):
        _banks(esp)
        esp.wait_state(
            bankname=name.replace(" ", "_"),
            bankoccupied=0,
        )

    _banks(esp)
    esp.page("NAME", first)
    _operation(esp, daisy, "New", 1, record_property)
    save(first)
    _banks(esp)
    esp.page("NAME", second)
    _operation(esp, daisy, "New", 1, record_property)
    # Saving a retained Project must capture the newly selected Bank.
    save(second)
    load(first)
    bank_name(first)
    load(second)
    bank_name(second)
    _files(esp)
    _confirmed(esp, "New", 3)
    bank_name("")
    save(empty)
    load(first)
    bank_name(first)
    load(empty)
    bank_name("")
