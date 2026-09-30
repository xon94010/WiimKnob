#pragma once

namespace input {

// The knob is not a quadrature encoder: each direction pulses its own GPIO low, one pulse per
// detent (verified by tracing the raw pin states -- see
// https://github.com/teetotum-rs/firmware/blob/main/docs/hardware/input.md, "The knob is not a
// quadrature encoder"). GPIO8 pulsing low is one clockwise detent, GPIO7 one counter-clockwise.
namespace knob {

void begin();

// Net detents (positive = clockwise) since the last call. Resets the counter.
int consumeDelta();

} // namespace knob
} // namespace input
