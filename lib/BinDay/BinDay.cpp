#include "BinDay.h"

#include <ctype.h>

static bool startsWithIgnoreCase(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

BinType binTypeFromPayload(const char *payload) {
    if (payload == nullptr) return BIN_UNKNOWN;

    while (*payload == ' ') payload++;  // the friendly names carry stray spaces

    if (startsWithIgnoreCase(payload, "landfill")) return BIN_LANDFILL;
    if (startsWithIgnoreCase(payload, "recycl")) return BIN_RECYCLE;
    return BIN_UNKNOWN;
}
