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

#ifndef LOCALAPISERVER_H
#define LOCALAPISERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QMutex>
#include <QStringList>

#include <atomic>
#include <deque>
#include <vector>

#include "types.h"

namespace melonDS
{
class NDS;
}

// Read-only HTTP API on 127.0.0.1 so local tools (such as a run tracker in a
// browser) can read emulated memory. The server lives on the GUI thread; memory
// is read on the emu thread between frames, so every read sees a whole frame.
//
//   GET /status                          -> JSON: game code, title, frame
//   GET /read?ranges=ADDR:LEN[,ADDR:LEN] -> the bytes of each range, back to back
//   GET /watch?ranges=ADDR:LEN[,...]     -> start recording those ranges every frame
//   GET /changes?since=SEQ               -> JSON: each frame where the watched bytes changed
//
// ADDR is hex (0x optional) and must be in main RAM (0x02000000-0x02FFFFFF).
// LEN is decimal, or hex with 0x. At most kMaxReadBytes are returned per request.
class LocalAPIServer : public QObject
{
    Q_OBJECT

public:
    static constexpr int kDefaultPort = 31124;
    static constexpr melonDS::u32 kMaxReadBytes = 0x10000;
    static constexpr melonDS::u32 kMaxWatchBytes = 0x400;
    static constexpr size_t kMaxChanges = 4096;
    static constexpr size_t kMaxChangesPerResponse = 256;

    explicit LocalAPIServer(int instanceID, QObject* parent = nullptr);
    ~LocalAPIServer() override;

    // GUI thread. Returns false if the port could not be opened.
    bool start(int port, const QString& allowedOrigins);
    void stop();
    bool isListening() const { return server.isListening(); }
    int port() const { return server.serverPort(); }

    // Emu thread, once per loop iteration.
    void service(melonDS::NDS* nds, bool active, bool running);

signals:
    void jobsDone();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void onJobsDone();

private:
    struct Range
    {
        melonDS::u32 addr;
        melonDS::u32 len;
    };

    enum JobKind
    {
        job_Status,
        job_Read,
        job_Watch,
        job_Changes,
    };

    struct Job
    {
        quint64 id;
        JobKind kind;
        std::vector<Range> ranges;
        melonDS::u64 since = 0;

        // filled in on the emu thread
        int status = 200;
        QByteArray contentType;
        QByteArray body;
        melonDS::u32 frame = 0;
    };

    struct Client
    {
        QByteArray buffer;
        quint64 pendingJob = 0;
        QByteArray corsHeaders;
        bool keepAlive = true;
    };

    void processBuffer(QTcpSocket* socket);
    void handleRequest(QTcpSocket* socket, const QByteArray& method, const QByteArray& target,
                       const QHash<QByteArray, QByteArray>& headers);
    void sendResponse(QTcpSocket* socket, int status, const QByteArray& contentType,
                      const QByteArray& body, const QByteArray& extraHeaders = QByteArray());
    void sendError(QTcpSocket* socket, int status, const char* message);

    bool hostAllowed(const QByteArray& host) const;
    bool originAllowed(const QByteArray& origin) const;
    QByteArray corsHeadersFor(const QByteArray& origin) const;
    static bool parseRanges(const QString& spec, std::vector<Range>& out, QString& error,
                            melonDS::u32 maxBytes = kMaxReadBytes);

    void runJob(Job& job, melonDS::NDS* nds, bool active, bool running);
    void sampleWatch(melonDS::NDS* nds);
    QByteArray changesBody(melonDS::u64 since, melonDS::NDS* nds) const;

    int instanceID;
    QTcpServer server;
    QStringList origins;
    bool anyOrigin = false;

    QHash<QTcpSocket*, Client> clients;
    QHash<quint64, QTcpSocket*> jobSockets;
    quint64 nextJobID = 1;

    QMutex jobMutex;
    std::vector<Job> pendingJobs;
    std::vector<Job> doneJobs;
    std::atomic<bool> hasPending {false};
    // Set by stop() so the emu thread drops the watch the next time round
    std::atomic<bool> dropWatch {false};

    // Watched ranges and their change log, only touched on the emu thread.
    // Each frame where the bytes differ from the frame before is logged, so a
    // client polling now and then still sees every value they went through.
    struct Change
    {
        melonDS::u64 seq;
        melonDS::u32 frame;
        QByteArray data;
    };

    std::vector<Range> watchRanges;
    melonDS::u32 watchID = 0;
    QByteArray watchLast;
    melonDS::u32 watchLastFrame = 0;
    melonDS::NDS* watchNDS = nullptr;
    std::deque<Change> changes;
    melonDS::u64 nextSeq = 1;
};

#endif // LOCALAPISERVER_H
