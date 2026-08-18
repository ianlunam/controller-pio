#pragma once

#include <time.h>

// Kerbside collection schedule.
//
// Deliberately free of any Arduino or hardware dependency so it compiles for
// the host and can be tested with `pio test -e native`, which is a great deal
// faster than flashing and waiting for a Thursday.

// 2024-09-26 was a landfill collection day. Recycle runs the same fortnightly
// cycle offset by a week.
constexpr int BIN_EPOCH_YEAR  = 2024;
constexpr int BIN_EPOCH_MONTH = 9;
constexpr int BIN_EPOCH_DAY   = 26;
constexpr int BIN_CYCLE_DAYS  = 14;

enum BinType { BIN_LANDFILL, BIN_RECYCLE };

// Whole days from the bin epoch to `nowLocal`. Both ends are normalised to
// local midnight so the count ticks over at midnight rather than at midday,
// and rounding absorbs the one hour skew across a daylight saving boundary.
int daysSinceBinEpoch(const struct tm &nowLocal);

// Which bin goes out next, which is what the single coloured circle shows:
// red for landfill, yellow for recycle. Collection day itself counts as zero
// days away, so the circle holds the colour of the bin due that morning and
// only flips once the day is over.
BinType nextBinType(int daysSinceEpoch);
