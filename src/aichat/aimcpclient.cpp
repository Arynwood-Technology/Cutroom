/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "aimcpclient.h"
#include "aitoolutils.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace AiChat {

namespace {
// Listing tools is a quick lookup; a tool call can legitimately run for minutes (rendering, scene detection, transcription).
constexpr int s_listTimeoutMs = 15000;
constexpr int s_callTimeoutMs = 10 * 60 * 1000;
} // namespace

McpClient::McpClient(QObject *parent)
    : QObject(parent)
{
}

void McpClient::setUrl(const QUrl &url)
{
    m_url = url;
}

QUrl McpClient::url() const
{
    return m_url;
}

void McpClient::listTools(const ToolsCallback &done)
{
    post(QStringLiteral("tools/list"), QJsonObject(), s_listTimeoutMs, [done](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            done(false, QJsonArray(), error);
            return;
        }
        done(true, result.value(QLatin1String("tools")).toArray(), QString());
    });
}

QNetworkReply *McpClient::callTool(const QString &name, const QJsonObject &arguments, const CallCallback &done)
{
    QJsonObject params;
    params.insert(QStringLiteral("name"), name);
    params.insert(QStringLiteral("arguments"), arguments);
    return post(QStringLiteral("tools/call"), params, s_callTimeoutMs, [done](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            done(false, error);
            return;
        }
        bool isError = false;
        const QString text = resultText(result, &isError);
        done(!isError && !reportsFailure(text), text);
    });
}

QNetworkReply *McpClient::post(const QString &method, const QJsonObject &params, int timeoutMs,
                               const std::function<void(const QJsonObject &result, const QString &error)> &done)
{
    const int id = m_nextId++;
    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("id"), id);
    request.insert(QStringLiteral("method"), method);
    request.insert(QStringLiteral("params"), params);

    QNetworkRequest networkRequest(m_url);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // The streamable-HTTP transport rejects a client that does not accept both.
    networkRequest.setRawHeader("Accept", "application/json, text/event-stream");
    networkRequest.setTransferTimeout(timeoutMs);

    QNetworkReply *reply = m_manager.post(networkRequest, QJsonDocument(request).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, done]() {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            done(QJsonObject(), QStringLiteral("Cancelled"));
            return;
        }
        QJsonObject response;
        QString parseError;
        if (!parseResponse(body, reply->header(QNetworkRequest::ContentTypeHeader).toString(), id, &response, &parseError)) {
            if (reply->error() != QNetworkReply::NoError) {
                done(QJsonObject(), QStringLiteral("Can't reach the tool server at %1: %2").arg(m_url.toString(), reply->errorString()));
            } else {
                done(QJsonObject(), parseError);
            }
            return;
        }
        if (response.contains(QLatin1String("error"))) {
            const QJsonObject rpcError = response.value(QLatin1String("error")).toObject();
            done(QJsonObject(), rpcError.value(QLatin1String("message")).toString(QStringLiteral("The tool server returned an error")));
            return;
        }
        done(response.value(QLatin1String("result")).toObject(), QString());
    });
    return reply;
}

bool McpClient::parseResponse(const QByteArray &body, const QString &contentType, int id, QJsonObject *response, QString *error)
{
    QList<QByteArray> payloads;
    if (contentType.contains(QLatin1String("text/event-stream"), Qt::CaseInsensitive)) {
        // One event is the run of "data:" lines up to the next blank line.
        QByteArray data;
        const QList<QByteArray> lines = body.split('\n');
        for (QByteArray line : lines) {
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (line.isEmpty()) {
                if (!data.isEmpty()) {
                    payloads << data;
                    data.clear();
                }
            } else if (line.startsWith("data:")) {
                QByteArray value = line.mid(5);
                if (value.startsWith(' ')) {
                    value.remove(0, 1);
                }
                if (!data.isEmpty()) {
                    data += '\n';
                }
                data += value;
            }
        }
        if (!data.isEmpty()) {
            payloads << data;
        }
    } else {
        payloads << body;
    }
    for (const QByteArray &payload : payloads) {
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject object = doc.object();
        if (object.value(QLatin1String("id")).toInt(-1) == id && (object.contains(QLatin1String("result")) || object.contains(QLatin1String("error")))) {
            *response = object;
            return true;
        }
    }
    if (error) {
        *error = body.trimmed().isEmpty() ? QStringLiteral("The tool server sent an empty reply")
                                          : QStringLiteral("The tool server sent a reply that isn't a valid MCP response");
    }
    return false;
}

QString McpClient::resultText(const QJsonObject &result, bool *isError)
{
    if (isError) {
        *isError = result.value(QLatin1String("isError")).toBool();
    }
    QStringList parts;
    const QJsonArray content = result.value(QLatin1String("content")).toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QLatin1String("type")).toString();
        if (type == QLatin1String("text")) {
            parts << item.value(QLatin1String("text")).toString();
        } else if (!type.isEmpty()) {
            parts << QStringLiteral("[%1 content not shown]").arg(type);
        }
    }
    if (parts.isEmpty() && result.contains(QLatin1String("structuredContent"))) {
        parts << QString::fromUtf8(QJsonDocument(result.value(QLatin1String("structuredContent")).toObject()).toJson(QJsonDocument::Compact));
    }
    return parts.isEmpty() ? QStringLiteral("(no output)") : parts.join(QLatin1Char('\n'));
}

} // namespace AiChat
