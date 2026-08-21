#pragma once

// Parsing for inbound MQTT payloads. No Arduino dependency, so it compiles for
// the host and can be tested with `pio test -e native`.

// Parse a decimal integer, tolerating a trailing fraction ("12.5" -> 12).
// Returns false, leaving `out` untouched, for the "unknown" / "unavailable" /
// "" payloads Home Assistant publishes when an entity has no value. std::stoi
// threw on those, and an uncaught throw reboots the board.
bool parseInt(const char *s, int &out);
