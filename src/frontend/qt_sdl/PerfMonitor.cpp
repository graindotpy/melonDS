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

#include <math.h>

#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "PerfMonitor.h"

using namespace melonDS;

namespace
{
double percent(double part, double whole)
{
    return whole > 0 ? round(part * 1000.0 / whole) / 10.0 : 0;
}

double ms(double v)
{
    return round(v * 100.0) / 100.0;
}

const char* rendererName(int renderer)
{
    switch (renderer)
    {
    case 0: return "software";
    case 1: return "opengl";
    case 2: return "opengl-compute";
    default: return "unknown";
    }
}
}

void PerfMonitor::reset()
{
    cur = Second();
    seconds.clear();
}

void PerfMonitor::addFrame(const Frame& frame, double now)
{
    if (cur.frames == 0)
    {
        cur.time = QDateTime::currentDateTimeUtc();
        cur.start = now - frame.loopMs / 1000.0;
        cur.overclock = settings.overclock;
    }

    cur.frames++;
    cur.lastFrame = frame.number;
    if (frame.input) cur.input++;
    if (frame.flushed) cur.flushed++;
    if (frame.fastForward) cur.fastForward++;
    if (frame.slowMo) cur.slowMo++;
    cur.emuMs += frame.emuMs;
    cur.emuMaxMs = std::max(cur.emuMaxMs, frame.emuMs);
    cur.drawMs += frame.drawMs;
    cur.slowestMs = std::max(cur.slowestMs, frame.loopMs);
    cur.arm9Cycles += frame.arm9Cycles;
    cur.arm9Idle += frame.arm9Idle;
    if (frame.arm9Cycles)
        cur.cpuMax = std::max(cur.cpuMax, 100.0 * (frame.arm9Cycles - frame.arm9Idle) / frame.arm9Cycles);
    cur.end = now;

    if (now - cur.start >= 1.0)
    {
        seconds.push_back(cur);
        if (seconds.size() > kMaxSeconds)
            seconds.pop_front();
        cur = Second();
    }
}

QString PerfMonitor::titleSuffix() const
{
    if (seconds.empty()) return QString();
    const Second& s = seconds.back();
    return QString(" | game %1% | DS CPU %2%%3")
        .arg((int)round(percent(s.input, s.frames)))
        .arg((int)round(percent(s.arm9Cycles - s.arm9Idle, s.arm9Cycles)))
        .arg(s.overclock ? " (overclocked)" : "");
}

QByteArray PerfMonitor::json() const
{
    QJsonObject cfg;
    cfg["jit"] = settings.jit;
    cfg["renderer"] = rendererName(settings.renderer);
    cfg["threaded3D"] = settings.threaded3D;
    cfg["limitFPS"] = settings.limitFPS;
    cfg["audioSync"] = settings.audioSync;
    cfg["overclock"] = settings.overclock;
    cfg["console"] = settings.consoleType == 1 ? "dsi" : "ds";

    QJsonArray list;
    for (const Second& s : seconds)
    {
        double secs = s.end - s.start;
        QJsonObject o;
        o["time"] = s.time.toString(Qt::ISODate);
        o["frame"] = (qint64)s.lastFrame;
        // Frames emulated per second; 60 is full speed
        o["fps"] = secs > 0 ? round(s.frames * 10.0 / secs) / 10.0 : 0;
        // Share of frames where the game ran its update (read the buttons).
        // Below 100 with fps at 60 is slowdown inside the game.
        o["game"] = percent(s.input, s.frames);
        o["frames3D"] = percent(s.flushed, s.frames);
        // How busy the DS's main CPU was; near 100 means it can't keep up
        o["cpu"] = percent(s.arm9Cycles - s.arm9Idle, s.arm9Cycles);
        o["cpuMax"] = round(s.cpuMax * 10.0) / 10.0;
        // Time the PC took per frame
        o["emuMs"] = ms(s.frames ? s.emuMs / s.frames : 0);
        o["emuMaxMs"] = ms(s.emuMaxMs);
        o["drawMs"] = ms(s.frames ? s.drawMs / s.frames : 0);
        o["slowestMs"] = ms(s.slowestMs);
        o["fastForward"] = percent(s.fastForward, s.frames);
        if (s.slowMo) o["slowMo"] = percent(s.slowMo, s.frames);
        o["overclock"] = s.overclock;
        list.append(o);
    }

    QJsonObject obj;
    obj["settings"] = cfg;
    obj["seconds"] = list;
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}
