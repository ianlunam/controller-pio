#pragma once

// Which bin the coloured circle on the screen should show.
//
// The schedule itself belongs to Home Assistant, as the sensor.bin_day template
// sensor, and arrives here over MQTT. This only maps that sensor's payload onto
// something drawable. Keeping the schedule in exactly one place matters: the
// firmware used to derive it independently from an epoch date, with the
// opposite phase convention to the template, and so disagreed with Home
// Assistant on every Thursday for about two years.
enum BinType { BIN_UNKNOWN, BIN_LANDFILL, BIN_RECYCLE };

// Map a sensor.bin_day payload onto a bin.
//
// The template publishes "Landfill" and "Recycles". Matching is
// case-insensitive and on a prefix, so "Recycle" and "Recycling" work too if
// the template is ever reworded. Returns BIN_UNKNOWN for "unknown",
// "unavailable", an empty payload or anything unrecognised, so the caller can
// hold the last good value rather than blanking the display.
BinType binTypeFromPayload(const char *payload);
