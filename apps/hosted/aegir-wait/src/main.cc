/*
 * wait: wait for a period, or until a time of day (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga's Wait is a C: program (C:WAIT), template
 * WAIT [<n>] [SEC|SECS] [MIN|MINS] [UNTIL <time>]. With no period it waits one
 * second; UNTIL waits until the given time of day, read from the wall clock.
 * The sleep is the timer's (aegir/timer.h), through the runtime's nanosleep.
 */

#include <aegir/args.h>
#include <aegir/command.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

/* The seconds from a decimal string, or false when it is not one. */
bool seconds_of(char const *text, uint64_t &out) noexcept
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    uint64_t value = 0;
    for (char const *c = text; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(*c - '0');
    }
    out = value;
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("wait")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("WAIT/N,SEC=SECS/S,MIN=MINS/S,UNTIL/K", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "wait: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }

    uint64_t seconds = 1; /* the Amiga's default period */
    if (args.value("UNTIL") != nullptr) {
        int hours = 0;
        int minutes = 0;
        if (std::sscanf(args.value("UNTIL"), "%d:%d", &hours, &minutes) != 2 ||
            hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
            std::fprintf(stderr, "wait: UNTIL wants a time like 21:15\n");
            return 10;
        }
        struct timespec now {};
        if (::clock_gettime(CLOCK_REALTIME, &now) != 0) {
            std::fprintf(stderr, "wait: no clock\n");
            return 10;
        }
        long const second_of_day = static_cast<long>(now.tv_sec % 86400);
        long delta = (hours * 3600 + minutes * 60) - second_of_day;
        if (delta <= 0) {
            delta += 86400; /* already passed today: the next one */
        }
        seconds = static_cast<uint64_t>(delta);
    } else {
        uint64_t period = 1;
        if (args.value("WAIT") != nullptr && !seconds_of(args.value("WAIT"), period)) {
            std::fprintf(stderr, "wait: WAIT wants a number of seconds\n");
            return 10;
        }
        if (args.present("MIN") || args.present("MINS")) {
            period *= 60;
        }
        seconds = period;
    }

    struct timespec delay {};
    delay.tv_sec = static_cast<long>(seconds);
    delay.tv_nsec = 0;
    if (::nanosleep(&delay, nullptr) != 0) {
        std::fprintf(stderr, "wait: no timer\n");
        return 10;
    }
    return 0;
}
