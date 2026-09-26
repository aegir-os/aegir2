/*
 * date: print the date and time (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga's Date is a C: program, not a shell word, and it prints the whole
 * date and time -- AmigaDOS has no separate Time command. The time is UTC:
 * the image carries no timezone or locale tables, so the command formats the
 * civil date itself (Howard Hinnant's algorithm) rather than libc's.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {

char const *const kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
char const *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* Civil date from days since the epoch (Howard Hinnant's algorithm). 1970-01-01
 * was a Thursday. */
void civil_from_days(long days, int &year, unsigned &month, unsigned &day) noexcept
{
    long const z = days + 719468;
    long const era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned const doe = static_cast<unsigned>(z - era * 146097);
    unsigned const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long const y = static_cast<long>(yoe) + era * 400;
    unsigned const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned const mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    year = static_cast<int>(y + (month <= 2));
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("date")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("DATE", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "date: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    struct timespec now {};
    if (::clock_gettime(CLOCK_REALTIME, &now) != 0) {
        std::fprintf(stderr, "date: no clock\n");
        return 10;
    }
    long const seconds = static_cast<long>(now.tv_sec);
    long day = seconds / 86400;
    long rem = seconds % 86400;
    if (rem < 0) {
        day -= 1;
        rem += 86400;
    }
    int year = 0;
    unsigned month = 1;
    unsigned day_of_month = 1;
    civil_from_days(day, year, month, day_of_month);
    uint32_t const weekday = static_cast<uint32_t>(((day % 7) + 4 + 7) % 7);
    std::printf("%s %02u-%s-%02d %02ld:%02ld:%02ld\n", kWeekdays[weekday], day_of_month,
                kMonths[month - 1], year % 100, rem / 3600, (rem / 60) % 60, rem % 60);
    return 0;
}
