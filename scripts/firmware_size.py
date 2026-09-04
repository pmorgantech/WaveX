#!/usr/bin/env python3
"""Measure firmware image sizes and (optionally) append them to the size log.

Usage:
    scripts/firmware_size.py                  # print a row per image found
    scripts/firmware_size.py --record "note"  # also append to the size log
    scripts/firmware_size.py --daisy-build build-stageb --label "daisy B"

Measures the Daisy ELF/BIN under firmware/daisy/<build>/ and, when present,
the ESP32 app binary under firmware/esp32/build/. Flash for the Daisy is
text + data (the .data image is stored in QSPI and copied to SRAM at boot);
RAM is data + bss. Percentages are against the STM32H750's 512 KiB AXI SRAM,
which is where libDaisy's QSPI layout puts both.

Run it from the devcontainer after the build you want to measure; the log row
records the commit the tree is at, so build and record from a clean tree
(a "+" suffix marks a dirty one).
"""

import argparse
import datetime as dt
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOG = ROOT / "docs" / "firmware-size-log.md"
DAISY_SRAM_BYTES = 512 * 1024
DAISY_QSPI_BYTES = 7936 * 1024

HEADER = (
    "| Date | Commit | Image | text | data | bss | Flash | RAM | "
    "Regions | Note |\n"
    "|---|---|---|---:|---:|---:|---:|---:|---|---|\n"
)

# STM32H750 memory map as libDaisy's linker scripts name it (VMA ranges).
# Used to attribute the ELF's allocated sections to regions, which is the
# same accounting the linker's --print-memory-usage line does.
DAISY_REGIONS = (
    ("ITCM", 0x00000000, 64 * 1024),
    ("DTCM", 0x20000000, 128 * 1024),
    ("SRAM", 0x24000000, 512 * 1024),
    ("D2DMA", 0x30000000, 32 * 1024),
    ("D2", 0x30008000, 256 * 1024),
    ("D3", 0x38000000, 64 * 1024),
    ("BKP", 0x38800000, 4 * 1024),
    ("QSPI", 0x90040000, 7936 * 1024),
    ("SDRAM", 0xC0000000, 64 * 1024 * 1024),
)


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()


def describe_tree():
    commit = git("rev-parse", "--short", "HEAD")
    dirty = git("status", "--porcelain", "--untracked-files=no") != ""
    return commit + ("+" if dirty else "")


def arm_size(elf):
    out = (
        subprocess.run(
            ["arm-none-eabi-size", str(elf)],
            capture_output=True,
            text=True,
            check=True,
        )
        .stdout.splitlines()[-1]
        .split()
    )
    text, data, bss = (int(v) for v in out[:3])
    return text, data, bss


def daisy_regions(elf):
    """Bytes of allocated sections per memory region, by VMA."""
    out = subprocess.run(
        ["arm-none-eabi-readelf", "-S", "-W", str(elf)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    used = {name: 0 for name, _, _ in DAISY_REGIONS}
    for line in out.splitlines():
        m = re.match(
            r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+"
            r"([0-9a-f]+)\s+\S+\s+([A-Za-z]*)\s",
            line,
        )
        if not m:
            continue
        name, kind, addr, size, flags = m.groups()
        if "A" not in flags or kind == "NULL":
            continue
        addr, size = int(addr, 16), int(size, 16)
        for region, base, length in DAISY_REGIONS:
            if base <= addr < base + length:
                used[region] += size
                break
    parts = []
    for region, _, length in DAISY_REGIONS:
        # Flash is the Flash column; RAM regions are the interesting split.
        if not used[region] or region in ("QSPI", "SDRAM"):
            continue
        pct = 100.0 * used[region] / length
        if used[region] < 1024:
            parts.append(f"{region} {used[region]}B")
        else:
            parts.append(f"{region} {used[region] / 1024:.1f}K ({pct:.0f}%)")
    return " / ".join(parts)


def daisy_row(build_dir, label):
    elf = ROOT / "firmware" / "daisy" / build_dir / "wavex-daisy.elf"
    if not elf.exists():
        return None
    text, data, bss = arm_size(elf)
    binary = elf.with_suffix(".bin")
    flash = text + data
    # `make bin` writes the flashable image; when it is absent, text + data is
    # what objcopy -O binary would produce (the sections are contiguous).
    image = binary.stat().st_size if binary.exists() else flash
    ram = data + bss
    return {
        "image": label,
        "text": text,
        "data": data,
        "bss": bss,
        "flash": f"{image} ({100.0 * image / DAISY_QSPI_BYTES:.1f}%)",
        "ram": f"{ram} ({100.0 * ram / DAISY_SRAM_BYTES:.1f}%)",
        "regions": daisy_regions(elf),
    }


def esp32_row():
    build = ROOT / "firmware" / "esp32" / "build"
    bins = sorted(build.glob("*.bin")) if build.exists() else []
    skip = ("bootloader.bin", "partition-table.bin", "ota_data_initial.bin")
    app = [b for b in bins if b.name not in skip]
    if not app:
        return None
    app_bin = app[0]
    return {
        "image": "esp32 app",
        "text": "",
        "data": "",
        "bss": "",
        "flash": str(app_bin.stat().st_size),
        "ram": "",
        "regions": "",
    }


def fmt_row(date, commit, row, note):
    return (
        f"| {date} | {commit} | {row['image']} | {row['text']} | "
        f"{row['data']} | {row['bss']} | {row['flash']} | {row['ram']} | "
        f"{row['regions']} | {note} |\n"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--record",
        metavar="NOTE",
        help="append rows to the size log",
    )
    ap.add_argument(
        "--daisy-build",
        default="build",
        help="Daisy build dir (default build)",
    )
    ap.add_argument(
        "--label",
        default="daisy",
        help="image label for the Daisy row",
    )
    ap.add_argument("--commit", help="override the commit column (backfill)")
    ap.add_argument("--date", help="override the date column (backfill)")
    ap.add_argument(
        "--no-esp32",
        action="store_true",
        help="skip the ESP32 row",
    )
    ap.add_argument(
        "--no-daisy",
        action="store_true",
        help="skip the Daisy row",
    )
    args = ap.parse_args()

    rows = []
    if not args.no_daisy:
        daisy = daisy_row(args.daisy_build, args.label)
        if daisy:
            rows.append(daisy)
    if not args.no_esp32:
        esp = esp32_row()
        if esp:
            rows.append(esp)
    if not rows:
        sys.exit("no firmware images found; build first")

    date = args.date or dt.date.today().isoformat()
    commit = args.commit or describe_tree()
    for r in rows:
        sys.stdout.write(fmt_row(date, commit, r, args.record or ""))

    if args.record is not None:
        text = LOG.read_text() if LOG.exists() else ""
        if HEADER not in text:
            sys.exit(f"{LOG} does not contain the expected table header")
        # Append after the last table row: the table is the final block.
        stripped = text.rstrip("\n") + "\n"
        if not re.search(r"\n\|[^\n]*\|\n$", stripped):
            sys.exit(f"{LOG} must end with the size table")
        with LOG.open("w") as f:
            f.write(stripped)
            for r in rows:
                f.write(fmt_row(date, commit, r, args.record))
        print(f"recorded {len(rows)} row(s) in {LOG.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
