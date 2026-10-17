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

#include <string.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

#include "LocalAPIServer.h"
#include "NDS.h"
#include "Platform.h"

using namespace melonDS;

namespace
{
constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr int kMaxClients = 32;
constexpr int kMaxRanges = 64;
constexpr u32 kMainRAMStart = 0x02000000;
constexpr u32 kMainRAMEnd = 0x03000000;

const char* reasonPhrase(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

QByteArray errorBody(const char* message)
{
    QJsonObject obj;
    obj["error"] = message;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}
}

LocalAPIServer::LocalAPIServer(int instanceID, QObject* parent)
    : QObject(parent), instanceID(instanceID)
{
    connect(&server, &QTcpServer::newConnection, this, &LocalAPIServer::onNewConnection);
    connect(this, &LocalAPIServer::jobsDone, this, &LocalAPIServer::onJobsDone, Qt::QueuedConnection);
}

LocalAPIServer::~LocalAPIServer()
{
    stop();
}

bool LocalAPIServer::start(int port, const QString& allowedOrigins)
{
    stop();

    origins.clear();
    anyOrigin = false;
    for (const QString& entry : allowedOrigins.split(',', Qt::SkipEmptyParts))
    {
        QString origin = entry.trimmed();
        if (origin == "*")
            anyOrigin = true;
        else if (!origin.isEmpty())
            origins.append(origin);
    }

    if (!server.listen(QHostAddress::LocalHost, port))
    {
        Platform::Log(Platform::LogLevel::Error, "Local API: could not listen on 127.0.0.1:%d: %s\n",
                      port, server.errorString().toUtf8().constData());
        return false;
    }

    Platform::Log(Platform::LogLevel::Info, "Local API: listening on 127.0.0.1:%d\n", port);
    return true;
}

void LocalAPIServer::stop()
{
    if (server.isListening())
        server.close();

    for (QTcpSocket* socket : clients.keys())
    {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    clients.clear();
    jobSockets.clear();

    QMutexLocker lock(&jobMutex);
    pendingJobs.clear();
    doneJobs.clear();
    hasPending = false;
}

void LocalAPIServer::onNewConnection()
{
    while (server.hasPendingConnections())
    {
        QTcpSocket* socket = server.nextPendingConnection();
        if (clients.size() >= kMaxClients)
        {
            socket->abort();
            socket->deleteLater();
            continue;
        }

        clients.insert(socket, Client());
        connect(socket, &QTcpSocket::readyRead, this, &LocalAPIServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &LocalAPIServer::onDisconnected);
    }
}

void LocalAPIServer::onReadyRead()
{
    auto socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !clients.contains(socket)) return;

    clients[socket].buffer += socket->readAll();
    processBuffer(socket);
}

void LocalAPIServer::onDisconnected()
{
    auto socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    clients.remove(socket);
    for (auto it = jobSockets.begin(); it != jobSockets.end();)
    {
        if (it.value() == socket)
            it = jobSockets.erase(it);
        else
            ++it;
    }
    socket->deleteLater();
}

void LocalAPIServer::processBuffer(QTcpSocket* socket)
{
    // One request at a time per connection, so pipelined requests are
    // answered in order.
    while (clients.contains(socket) && clients[socket].pendingJob == 0)
    {
        Client& client = clients[socket];

        int end = client.buffer.indexOf("\r\n\r\n");
        if (end < 0)
        {
            if (client.buffer.size() > kMaxHeaderBytes)
            {
                client.keepAlive = false;
                sendError(socket, 431, "Request headers too large");
            }
            return;
        }

        QByteArray head = client.buffer.left(end);
        client.buffer.remove(0, end + 4);

        QList<QByteArray> lines = head.split('\n');
        QList<QByteArray> requestLine = lines.takeFirst().trimmed().split(' ');
        if (requestLine.size() != 3)
        {
            client.keepAlive = false;
            sendError(socket, 400, "Malformed request line");
            return;
        }

        QHash<QByteArray, QByteArray> headers;
        for (const QByteArray& line : lines)
        {
            int colon = line.indexOf(':');
            if (colon <= 0) continue;
            headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
        }

        QByteArray connection = headers.value("connection").toLower();
        if (requestLine[2] == "HTTP/1.0")
            client.keepAlive = connection == "keep-alive";
        else
            client.keepAlive = connection != "close";

        // Only bodiless requests are supported.
        if (headers.value("content-length", "0").toLongLong() != 0 || headers.contains("transfer-encoding"))
        {
            client.keepAlive = false;
            sendError(socket, 400, "Request bodies are not supported");
            return;
        }

        handleRequest(socket, requestLine[0], requestLine[1], headers);
    }
}

void LocalAPIServer::handleRequest(QTcpSocket* socket, const QByteArray& method, const QByteArray& target,
                                   const QHash<QByteArray, QByteArray>& headers)
{
    Client& client = clients[socket];
    client.corsHeaders.clear();

    // Refuse requests that reached us through a hostname other than loopback,
    // so a web page can't use DNS rebinding to read memory.
    if (!hostAllowed(headers.value("host")))
    {
        client.keepAlive = false;
        sendError(socket, 403, "Host not allowed");
        return;
    }

    QByteArray origin = headers.value("origin");
    if (!origin.isEmpty() && !originAllowed(origin))
    {
        sendError(socket, 403, "Origin not allowed");
        return;
    }
    client.corsHeaders = corsHeadersFor(origin);

    if (method == "OPTIONS")
    {
        QByteArray extra = "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                           "Access-Control-Max-Age: 600\r\n";
        QByteArray requestHeaders = headers.value("access-control-request-headers");
        if (!requestHeaders.isEmpty())
            extra += "Access-Control-Allow-Headers: " + requestHeaders + "\r\n";
        if (headers.value("access-control-request-private-network") == "true")
            extra += "Access-Control-Allow-Private-Network: true\r\n";

        sendResponse(socket, 204, QByteArray(), QByteArray(), extra);
        return;
    }

    if (method != "GET")
    {
        sendError(socket, 405, "Only GET is supported");
        return;
    }

    QUrl url(QString::fromLatin1(target));
    QString path = url.path();

    Job job;
    if (path == "/status")
    {
        job.kind = job_Status;
    }
    else if (path == "/read")
    {
        QString error;
        QUrlQuery query(url);
        if (!parseRanges(query.queryItemValue("ranges"), job.ranges, error))
        {
            sendError(socket, 400, error.toUtf8().constData());
            return;
        }
        job.kind = job_Read;
    }
    else
    {
        sendError(socket, 404, "Unknown endpoint");
        return;
    }

    job.id = nextJobID++;
    client.pendingJob = job.id;
    jobSockets.insert(job.id, socket);

    QMutexLocker lock(&jobMutex);
    pendingJobs.push_back(std::move(job));
    hasPending = true;
}

void LocalAPIServer::sendResponse(QTcpSocket* socket, int status, const QByteArray& contentType,
                                  const QByteArray& body, const QByteArray& extraHeaders)
{
    const Client& client = clients[socket];

    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + reasonPhrase(status) + "\r\n";
    if (!contentType.isEmpty())
        response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += client.corsHeaders;
    response += extraHeaders;
    response += client.keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    response += "\r\n";
    response += body;

    socket->write(response);
    if (!client.keepAlive)
        socket->disconnectFromHost();
}

void LocalAPIServer::sendError(QTcpSocket* socket, int status, const char* message)
{
    sendResponse(socket, status, "application/json", errorBody(message));
}

bool LocalAPIServer::hostAllowed(const QByteArray& host) const
{
    // HTTP/1.0 clients may leave it out; browsers always send it.
    if (host.isEmpty()) return true;

    QByteArray name = host.toLower();
    if (name.startsWith('['))
        name = name.left(name.indexOf(']') + 1);
    else if (name.contains(':'))
        name = name.left(name.lastIndexOf(':'));

    return name == "127.0.0.1" || name == "localhost" || name == "[::1]";
}

bool LocalAPIServer::originAllowed(const QByteArray& origin) const
{
    return anyOrigin || origins.contains(QString::fromLatin1(origin));
}

QByteArray LocalAPIServer::corsHeadersFor(const QByteArray& origin) const
{
    if (origin.isEmpty()) return QByteArray();

    QByteArray headers = anyOrigin
        ? QByteArray("Access-Control-Allow-Origin: *\r\n")
        : "Access-Control-Allow-Origin: " + origin + "\r\nVary: Origin\r\n";
    headers += "Access-Control-Expose-Headers: X-Melonds-Frame\r\n";
    return headers;
}

bool LocalAPIServer::parseRanges(const QString& spec, std::vector<Range>& out, QString& error)
{
    if (spec.isEmpty())
    {
        error = "Missing ranges, expected ranges=ADDR:LEN[,ADDR:LEN...]";
        return false;
    }

    QStringList parts = spec.split(',');
    if (parts.size() > kMaxRanges)
    {
        error = QString("At most %1 ranges per request").arg(kMaxRanges);
        return false;
    }

    u64 total = 0;
    for (const QString& part : parts)
    {
        QStringList fields = part.trimmed().split(':');
        bool okAddr = false, okLen = false;
        u32 addr = 0, len = 0;
        if (fields.size() == 2)
        {
            QString addrStr = fields[0];
            if (addrStr.startsWith("0x", Qt::CaseInsensitive))
                addrStr = addrStr.mid(2);
            addr = addrStr.toUInt(&okAddr, 16);
            len = fields[1].toUInt(&okLen, 0);
        }

        if (!okAddr || !okLen || len == 0)
        {
            error = "Bad range '" + part + "', expected ADDR:LEN";
            return false;
        }
        if (addr < kMainRAMStart || (u64)addr + len > kMainRAMEnd)
        {
            error = "Range '" + part + "' is outside main RAM (0x02000000-0x02FFFFFF)";
            return false;
        }

        total += len;
        if (total > kMaxReadBytes)
        {
            error = QString("At most %1 bytes per request").arg(kMaxReadBytes);
            return false;
        }

        out.push_back({addr, len});
    }

    return true;
}

void LocalAPIServer::service(NDS* nds, bool active, bool running)
{
    if (!hasPending.load()) return;

    std::vector<Job> jobs;
    {
        QMutexLocker lock(&jobMutex);
        jobs.swap(pendingJobs);
        hasPending = false;
    }

    for (Job& job : jobs)
        runJob(job, nds, active, running);

    {
        QMutexLocker lock(&jobMutex);
        for (Job& job : jobs)
            doneJobs.push_back(std::move(job));
    }
    emit jobsDone();
}

void LocalAPIServer::runJob(Job& job, NDS* nds, bool active, bool running)
{
    job.frame = nds ? nds->NumFrames : 0;

    if (job.kind == job_Status)
    {
        QJsonObject obj;
        obj["api"] = 1;
        obj["instance"] = instanceID;
        obj["active"] = active;
        obj["running"] = running;
        obj["frame"] = (qint64)job.frame;

        if (nds && nds->CartInserted())
        {
            const NDSHeader& header = nds->GetNDSCart()->GetHeader();
            QJsonObject game;
            game["code"] = QString::fromLatin1(header.GameCode, strnlen(header.GameCode, sizeof(header.GameCode)));
            game["title"] = QString::fromLatin1(header.GameTitle, strnlen(header.GameTitle, sizeof(header.GameTitle)));
            game["version"] = header.ROMVersion;
            obj["game"] = game;
        }
        else
            obj["game"] = QJsonValue::Null;

        job.contentType = "application/json";
        job.body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        return;
    }

    if (!nds || !active)
    {
        job.status = 503;
        job.contentType = "application/json";
        job.body = errorBody("Nothing is running");
        return;
    }

    u32 total = 0;
    for (const Range& range : job.ranges)
        total += range.len;

    job.contentType = "application/octet-stream";
    job.body.resize(total);
    char* dst = job.body.data();
    for (const Range& range : job.ranges)
    {
        for (u32 i = 0; i < range.len; i++)
            *dst++ = nds->MainRAM[(range.addr + i) & nds->MainRAMMask];
    }
}

void LocalAPIServer::onJobsDone()
{
    std::vector<Job> jobs;
    {
        QMutexLocker lock(&jobMutex);
        jobs.swap(doneJobs);
    }

    for (Job& job : jobs)
    {
        QTcpSocket* socket = jobSockets.take(job.id);
        if (!socket || !clients.contains(socket)) continue;

        clients[socket].pendingJob = 0;
        QByteArray extra = "X-Melonds-Frame: " + QByteArray::number(job.frame) + "\r\n";
        sendResponse(socket, job.status, job.contentType, job.body, extra);

        processBuffer(socket);
    }
}
