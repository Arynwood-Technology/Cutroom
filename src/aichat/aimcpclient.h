/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <functional>

namespace AiChat {

/** @class McpClient
 *  @brief Minimal client for an MCP server's streamable-HTTP endpoint (the mcp-kdenlive service), enough to list and call tools.
 *  The server is expected to run stateless, as mcp-kdenlive does when started with MCP_TRANSPORT=http, so no session handshake
 *  is done. Replies come back either as plain JSON or as a single server-sent event, and both are handled. */
class McpClient : public QObject
{
    Q_OBJECT
public:
    using ToolsCallback = std::function<void(bool ok, const QJsonArray &tools, const QString &error)>;
    using CallCallback = std::function<void(bool ok, const QString &text)>;

    explicit McpClient(QObject *parent = nullptr);

    void setUrl(const QUrl &url);
    QUrl url() const;

    /** @brief Fetch the server's tool definitions in their MCP form. */
    void listTools(const ToolsCallback &done);
    /** @brief Run a tool. @p ok is false for transport errors, protocol errors and errors the tool itself reported;
     *  @p text then holds the message, which is fit to hand to a model. The returned reply can be aborted. */
    QNetworkReply *callTool(const QString &name, const QJsonObject &arguments, const CallCallback &done);

    /** @brief Extract the JSON-RPC response with the given @p id from a reply body, whether JSON or a server-sent event. */
    static bool parseResponse(const QByteArray &body, const QString &contentType, int id, QJsonObject *response, QString *error);
    /** @brief Flatten the result of a tools/call into text; @p isError is set from the result's isError flag. */
    static QString resultText(const QJsonObject &result, bool *isError);

private:
    QNetworkReply *post(const QString &method, const QJsonObject &params, int timeoutMs,
                        const std::function<void(const QJsonObject &result, const QString &error)> &done);

    QNetworkAccessManager m_manager;
    QUrl m_url;
    int m_nextId{1};
};

} // namespace AiChat
