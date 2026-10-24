/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef PERFMONITOR_H
#define PERFMONITOR_H

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <deque>

#include "types.h"

// Keeps a second-by-second record of how the emulator is keeping up, to tell
// apart slowdown from the PC (frames take too long to emulate or draw) and
// slowdown inside the game (the DS's CPU can't finish its work in a frame, so
// the game only updates every other frame or so, as it would on a real DS).
//
// Emu thread only.
class PerfMonitor
{
public:
    static constexpr size_t kMaxSeconds = 600;

    struct Frame
    {
        melonDS::u32 number;     // the emulator's frame count
        double emuMs;            // time spent emulating it
        double drawMs;           // time spent drawing it
        double loopMs;           // time since the previous frame started, waiting included
        bool input;              // the game read the buttons
        bool flushed;            // the game finished a 3D frame
        melonDS::u64 arm9Cycles; // ARM9 cycles in the frame
        melonDS::u64 arm9Idle;   // of which it spent halted or asleep
        melonDS::u64 arm9GXStall; // stalled on a full geometry FIFO
        melonDS::u64 arm9DMA;    // held up by DMA
        bool fastForward;
        bool slowMo;
    };

    struct Settings
    {
        bool jit = false;
        int renderer = 0;
        bool threaded3D = false;
        bool limitFPS = false;
        bool audioSync = false;
        bool overclock = false;
        int consoleType = 0;
    };

    void addFrame(const Frame& frame, double now);
    void setSettings(const Settings& s) { settings = s; }
    void reset();
    // Drops the second in progress, so time spent paused isn't counted
    void pause() { cur = Second(); }

    // The last whole second, for the window title
    QString titleSuffix() const;
    // Every second kept, newest last
    QByteArray json() const;

private:
    struct Second
    {
        QDateTime time;
        double start = 0;
        double end = 0;
        melonDS::u32 lastFrame = 0;
        melonDS::u32 frames = 0;
        melonDS::u32 input = 0;
        melonDS::u32 flushed = 0;
        melonDS::u32 fastForward = 0;
        melonDS::u32 slowMo = 0;
        double emuMs = 0, emuMaxMs = 0;
        double drawMs = 0;
        double slowestMs = 0;
        melonDS::u64 arm9Cycles = 0, arm9Idle = 0, arm9GXStall = 0, arm9DMA = 0;
        double cpuMax = 0;
        bool overclock = false;
    };

    Second cur;
    std::deque<Second> seconds;
    Settings settings;
};

#endif // PERFMONITOR_H
