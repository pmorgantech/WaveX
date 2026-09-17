#!/usr/bin/env python3
"""Numerical experiments, not firmware or a production renderer.

Run in the WaveX devcontainer: python3 tools/vintage_sampler_math.py
Prints deterministic JSON for docs/features/vintage-sampler-math.md.
The clock experiment loops a synthetic tone through an exact continuous-time
one-pole response to a held DAC. It has no final anti-alias stage, capture
filter, sample file I/O, resonant filter, or production voice lifecycle.
"""

import json
import math
from dataclasses import dataclass

MU = 255.0
LOG_MU = math.log1p(MU)
ENGINE_RATE = 48000


def compress(x):
    x = max(-1.0, min(1.0, x))
    return math.copysign(math.log1p(MU * abs(x)) / LOG_MU, x)


def expand(y):
    return math.copysign(math.expm1(abs(y) * LOG_MU) / MU, y)


def quantize_symmetric(y):
    """Sign plus 7-bit magnitude: 255 levels, two zero codes possible."""
    magnitude = min(127, math.floor(127.0 * abs(y) + 0.5))
    return math.copysign(magnitude / 127.0, y)


def quantize_supplied(y):
    """The supplied NumPy-style ties-to-even, endpoint-inclusive mapping."""
    return max(0, min(255, round((y + 1.0) * 127.5))) / 127.5 - 1.0


def rms(values):
    return math.sqrt(math.fsum(x * x for x in values) / len(values))


def snr_db(source, output):
    error = rms([x - y for x, y in zip(source, output)])
    return 20.0 * math.log10(rms(source) / error)


@dataclass
class HeldDacOnePole:
    """Offline exact event timing at constant integer virtual clock rates.

    The source is periodic for measurement only. Frame zero samples the filter
    at t=0 with zero initial state and source[0] already driving its input.
    Each subsequent DAC edge changes the input after evolving the old hold.
    """

    source: tuple
    clock_hz: int
    cutoff_hz: float
    output_hz: int = ENGINE_RATE
    frame: int = 0
    next_edge: int = 1
    time: float = 0.0
    state: float = 0.0
    max_edges_per_frame: int = 0

    def __post_init__(self):
        if not self.source or not all(math.isfinite(x) for x in self.source):
            raise ValueError("A finite, nonempty periodic source is required")
        if self.clock_hz <= 0 or self.output_hz <= 0 or self.cutoff_hz <= 0:
            raise ValueError("Rates and cutoff must be positive")
        self.held = self.source[0]
        self.pole = 2.0 * math.pi * self.cutoff_hz

    def advance(self, target):
        # -expm1 preserves accuracy for intervals much shorter than the pole.
        blend = -math.expm1(-self.pole * (target - self.time))
        self.state += (self.held - self.state) * blend
        self.time = target

    def render(self, frames):
        output = []
        for _ in range(frames):
            edges = 0
            # Integer comparison avoids accumulating fractional-clock drift.
            limit = self.frame * self.clock_hz
            while self.next_edge * self.output_hz <= limit:
                self.advance(self.next_edge / self.clock_hz)
                self.held = self.source[self.next_edge % len(self.source)]
                self.next_edge += 1
                edges += 1
            self.max_edges_per_frame = max(self.max_edges_per_frame, edges)
            self.advance(self.frame / self.output_hz)
            output.append(self.state)
            self.frame += 1
        return output


def crossing_frequency(samples, rate):
    times = []
    for i in range(1, len(samples)):
        a, b = samples[i - 1], samples[i]
        if a <= 0.0 < b:
            times.append((i - 1 + (-a / (b - a))) / rate)
    return (len(times) - 1) / (times[-1] - times[0])


def log_cosh(x):
    if abs(x) < 20.0:
        return math.log1p(2.0 * math.sinh(x / 2.0) ** 2)
    return abs(x) + math.log1p(math.exp(-2.0 * abs(x))) - math.log(2.0)


def saturate(x, drive):
    # Unity small-signal slope; makeup level is a separate control.
    return math.tanh(drive * x) / drive


def antiderivative(x, drive):
    return log_cosh(drive * x) / (drive * drive)


def adaa_pair(a, b, drive):
    if abs(a - b) < 1e-7 * max(1.0, abs(a), abs(b)):
        return saturate((a + b) * 0.5, drive)
    return (antiderivative(a, drive) - antiderivative(b, drive)) / (a - b)


def main():
    # Separate arithmetic checks from claims about listening quality.
    sweep = [i / 8192.0 for i in range(-8192, 8193)]
    repaired = [expand(quantize_symmetric(compress(x))) for x in sweep]
    assert repaired[len(repaired) // 2] == 0.0
    assert all(a <= b for a, b in zip(repaired, repaired[1:]))
    assert max(abs(a + b) for a, b in zip(repaired, reversed(repaired))) == 0.0
    assert len(set(repaired)) == 255
    assert max(abs(expand(compress(x)) - x) for x in sweep) < 2e-15

    snr = []
    for level in (0, -20, -40, -60):
        amplitude = 10.0 ** (level / 20.0)
        omega = 2.0 * math.pi * 101 / 8192
        tone = [amplitude * math.sin(omega * i) for i in range(8192)]
        companded = [expand(quantize_symmetric(compress(x))) for x in tone]
        linear = [quantize_symmetric(x) for x in tone]
        snr.append(
            {
                "peak_dbfs": level,
                "companded_snr_db": round(snr_db(tone, companded), 3),
                "linear_sign_magnitude_snr_db": round(snr_db(tone, linear), 3),
            }
        )

    # Constant and step inputs have independent closed-form solutions.
    steady = HeldDacOnePole((1.0,), 55400, 8000)
    values = steady.render(100)
    assert (
        max(
            abs(v - (-math.expm1(-2.0 * math.pi * 8000 * i / ENGINE_RATE)))
            for i, v in enumerate(values)
        )
        < 1e-14
    )
    step = HeldDacOnePole((0.0, 1.0), 27700, 8000)
    values = step.render(3)
    elapsed = 2 / ENGINE_RATE - 1 / 27700
    expected = -math.expm1(-2.0 * math.pi * 8000 * elapsed)
    assert values[:2] == [0.0, 0.0] and abs(values[2] - expected) < 1e-14

    source = tuple(math.sin(2 * math.pi * 10 * k / 277) for k in range(277))
    clocks = []
    for semitones, clock in ((-12, 13850), (0, 27700), (12, 55400)):
        model = HeldDacOnePole(source, clock, 8000)
        complete = model.render(12000)
        chunked = HeldDacOnePole(source, clock, 8000)
        parts = []
        for size in (1, 47, 256, 3, 4096, 7597):
            parts.extend(chunked.render(size))
        assert complete == parts
        expected_hz = 1000.0 * clock / 27700
        measured_hz = crossing_frequency(complete[2400:], ENGINE_RATE)
        assert abs(measured_hz - expected_hz) < 0.1
        expected_edges = ((len(complete) - 1) * clock) // ENGINE_RATE
        assert model.next_edge - 1 == expected_edges
        clocks.append(
            {
                "semitones": semitones,
                "virtual_clock_hz": clock,
                "source_frames_per_output": clock / ENGINE_RATE,
                "measured_tone_hz": round(measured_hz, 6),
                "max_edges_per_output": model.max_edges_per_frame,
                "chunk_invariant": True,
            }
        )

    # Verify the proposed antiderivative and the equal-input limit, not its
    # spectral rejection or suitability for a target MCU.
    for x in (-1.0, -0.1, 0.0, 0.1, 1.0):
        h = 1e-5
        upper = antiderivative(x + h, 1.5)
        lower = antiderivative(x - h, 1.5)
        derivative = (upper - lower) / (2 * h)
        assert abs(derivative - saturate(x, 1.5)) < 1e-9
        assert adaa_pair(x, x, 1.5) == saturate(x, 1.5)

    capture_hz = 27700
    cutoff = 0.45 * capture_hz
    response = []
    for order in (4, 8):
        ratio = (capture_hz / 2) / cutoff
        response.append(
            {
                "butterworth_order": order,
                "analog_cutoff_hz": cutoff,
                "at_capture_nyquist_db": round(
                    -10 * math.log10(1 + ratio ** (2 * order)),
                    3,
                ),
            }
        )
    print(
        json.dumps(
            {
                "scope": (
                    "Analytical/numerical reference only; "
                    "no device or audio-file tests"
                ),
                "checks": "passed",
                "supplied_quantizer_zero": expand(quantize_supplied(0.0)),
                "symmetric_quantizer_zero": expand(quantize_symmetric(0.0)),
                "distinct_levels": len(set(repaired)),
                "snr": snr,
                "fixed_48000_output": clocks,
                "capture_filter": response,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
