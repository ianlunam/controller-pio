#include "MqttPayload.h"

#include <stdlib.h>

bool parseInt(const char *s, int &out) {
    if (s == nullptr) return false;

    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s) return false;

    out = static_cast<int>(v);
    return true;
}
