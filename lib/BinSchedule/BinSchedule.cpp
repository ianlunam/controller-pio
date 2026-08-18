#include "BinSchedule.h"

#include <math.h>

int daysSinceBinEpoch(const struct tm &nowLocal) {
    struct tm today = nowLocal;
    today.tm_hour = today.tm_min = today.tm_sec = 0;
    today.tm_isdst = -1;

    struct tm epoch = {};
    epoch.tm_year  = BIN_EPOCH_YEAR - 1900;
    epoch.tm_mon   = BIN_EPOCH_MONTH - 1;
    epoch.tm_mday  = BIN_EPOCH_DAY;
    epoch.tm_isdst = -1;

    time_t t1 = mktime(&today);
    time_t t2 = mktime(&epoch);
    return static_cast<int>(lround(difftime(t1, t2) / 86400.0));
}

BinType nextBinType(int daysSinceEpoch) {
    // Floored modulo, so a clock reading a date before the epoch still lands
    // in [0,14) instead of going negative.
    const int phase = ((daysSinceEpoch % BIN_CYCLE_DAYS) + BIN_CYCLE_DAYS) % BIN_CYCLE_DAYS;

    // Days until each collection, counting the collection day itself as zero.
    const int toLandfill = (BIN_CYCLE_DAYS - phase) % BIN_CYCLE_DAYS;
    const int toRecycle  = (BIN_CYCLE_DAYS + 7 - phase) % BIN_CYCLE_DAYS;

    return toLandfill < toRecycle ? BIN_LANDFILL : BIN_RECYCLE;
}
