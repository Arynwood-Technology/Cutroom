/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "aillmclient.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>

namespace AiChat {

namespace {

// The first token can be slow: a cold model has to be loaded into memory and a large tool list read before anything streams.
constexpr int s_chatTimeoutMs = 5 * 60 * 1000;
constexpr int s_listTimeoutMs = 10000;

QString roleName(Message::Role role)
{
    switch (role) {
    case Message::Role::System:
        return QStringLiteral("system");
    case Message::Role::User:
        return QStringLiteral("user");
    case Message::Role::Assistant:
        return QStringLiteral("assistant");
    case Message::Role::Tool:
        return QStringLiteral("tool");
    }
    return QStringLiteral("user");
}

QJsonObject messageJson(Provider provider, const Message &message)
{
    QJsonObject object;
    object.insert(QStringLiteral("role"), roleName(message.role));
    object.insert(QStringLiteral("content"), message.content);
    if (message.role == Message::Role::Assistant && !message.toolCalls.isEmpty()) {
        QJsonArray calls;
        for (const ToolCall &call : message.toolCalls) {
            QJsonObject function;
            function.insert(QStringLiteral("name"), call.name);
            QJsonObject entry;
            if (provider == Provider::Ollama) {
                function.insert(QStringLiteral("arguments"), call.arguments);
            } else {
                function.insert(QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact)));
                entry.insert(QStringLiteral("id"), call.id);
                entry.insert(QStringLiteral("type"), QStringLiteral("function"));
            }
            entry.insert(QStringLiteral("function"), function);
            calls.append(entry);
        }
        object.insert(QStringLiteral("tool_calls"), calls);
    } else if (message.role == Message::Role::Tool) {
        if (provider == Provider::Ollama) {
            object.insert(QStringLiteral("tool_name"), message.toolName);
        } else {
            object.insert(QStringLiteral("tool_call_id"), message.toolCallId);
        }
    }
    return object;
}

/** Ollama and OpenAI both report failures as JSON, but shape the message differently. */
QString errorFromBody(const QByteArray &body)
{
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    const QJsonValue error = object.value(QLatin1String("error"));
    if (error.isString()) {
        return error.toString();
    }
    if (error.isObject()) {
        return error.toObject().value(QLatin1String("message")).toString();
    }
    return QString::fromUtf8(body.left(300)).trimmed();
}

QJsonObject argumentsFromValue(const QJsonValue &value)
{
    if (value.isObject()) {
        return value.toObject();
    }
    // Some servers hand arguments over as a JSON-encoded string even where an object is documented.
    return QJsonDocument::fromJson(value.toString().toUtf8()).object();
}

} // namespace

ChatStreamParser::ChatStreamParser(Provider provider)
    : m_provider(provider)
{
}

QString ChatStreamParser::feed(const QByteArray &chunk)
{
    m_buffer += chunk;
    QString text;
    int newline;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        text += parseLine(line);
    }
    return text;
}

QString ChatStreamParser::finish()
{
    const QByteArray rest = m_buffer;
    m_buffer.clear();
    return parseLine(rest);
}

QString ChatStreamParser::parseLine(const QByteArray &rawLine)
{
    const QByteArray line = rawLine.trimmed();
    if (line.isEmpty()) {
        return {};
    }
    if (m_provider == Provider::Ollama) {
        return parseOllamaLine(QJsonDocument::fromJson(line).object());
    }
    if (!line.startsWith("data:")) {
        // Comments (": keep-alive") and other SSE fields carry nothing for us.
        return {};
    }
    const QByteArray payload = line.mid(5).trimmed();
    if (payload == "[DONE]") {
        return {};
    }
    return parseOpenAiPayload(QJsonDocument::fromJson(payload).object());
}

QString ChatStreamParser::parseOllamaLine(const QJsonObject &object)
{
    if (object.contains(QLatin1String("error"))) {
        m_error = object.value(QLatin1String("error")).toString();
        return {};
    }
    const QJsonObject message = object.value(QLatin1String("message")).toObject();
    const QString text = message.value(QLatin1String("content")).toString();
    m_content += text;
    const QJsonArray calls = message.value(QLatin1String("tool_calls")).toArray();
    for (const QJsonValue &value : calls) {
        const QJsonObject function = value.toObject().value(QLatin1String("function")).toObject();
        ToolCall call;
        call.name = function.value(QLatin1String("name")).toString();
        call.arguments = argumentsFromValue(function.value(QLatin1String("arguments")));
        m_ollamaCalls << call;
    }
    return text;
}

QString ChatStreamParser::parseOpenAiPayload(const QJsonObject &object)
{
    if (object.contains(QLatin1String("error"))) {
        const QJsonValue error = object.value(QLatin1String("error"));
        m_error = error.isObject() ? error.toObject().value(QLatin1String("message")).toString() : error.toString();
        return {};
    }
    const QJsonArray choices = object.value(QLatin1String("choices")).toArray();
    if (choices.isEmpty()) {
        return {};
    }
    const QJsonObject delta = choices.at(0).toObject().value(QLatin1String("delta")).toObject();
    const QString text = delta.value(QLatin1String("content")).toString();
    m_content += text;
    const QJsonArray calls = delta.value(QLatin1String("tool_calls")).toArray();
    for (const QJsonValue &value : calls) {
        const QJsonObject call = value.toObject();
        PartialCall &partial = m_openAiCalls[call.value(QLatin1String("index")).toInt(0)];
        if (call.contains(QLatin1String("id"))) {
            partial.id = call.value(QLatin1String("id")).toString();
        }
        const QJsonObject function = call.value(QLatin1String("function")).toObject();
        if (function.contains(QLatin1String("name"))) {
            partial.name += function.value(QLatin1String("name")).toString();
        }
        partial.arguments += function.value(QLatin1String("arguments")).toString();
    }
    return text;
}

QString ChatStreamParser::content() const
{
    return m_content;
}

QList<ToolCall> ChatStreamParser::toolCalls() const
{
    if (m_provider == Provider::Ollama) {
        return m_ollamaCalls;
    }
    QList<ToolCall> calls;
    for (auto it = m_openAiCalls.begin(); it != m_openAiCalls.end(); ++it) {
        ToolCall call;
        call.id = it.value().id;
        call.name = it.value().name;
        call.arguments = argumentsFromValue(it.value().arguments);
        calls << call;
    }
    return calls;
}

QString ChatStreamParser::error() const
{
    return m_error;
}

LlmClient::LlmClient(QObject *parent)
    : QObject(parent)
{
}

LlmClient::~LlmClient()
{
    abort();
}

void LlmClient::setProvider(Provider provider)
{
    m_provider = provider;
}

void LlmClient::setBaseUrl(const QUrl &url)
{
    m_baseUrl = url;
}

void LlmClient::setApiKey(const QString &key)
{
    m_apiKey = key;
}

bool LlmClient::isBusy() const
{
    return m_reply != nullptr;
}

QUrl LlmClient::endpoint(const QString &path) const
{
    QString base = m_baseUrl.toString();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return QUrl(base + path);
}

QNetworkRequest LlmClient::jsonRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_apiKey.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
    }
    return request;
}

QJsonObject LlmClient::requestBody(Provider provider, const LlmRequest &request)
{
    QJsonArray messages;
    for (const Message &message : request.messages) {
        messages.append(messageJson(provider, message));
    }
    QJsonObject body;
    body.insert(QStringLiteral("model"), request.model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), true);
    if (!request.tools.isEmpty()) {
        body.insert(QStringLiteral("tools"), request.tools);
    }
    if (provider == Provider::Ollama && request.contextSize > 0) {
        QJsonObject options;
        options.insert(QStringLiteral("num_ctx"), request.contextSize);
        body.insert(QStringLiteral("options"), options);
    }
    return body;
}

void LlmClient::listModels(const std::function<void(bool, const QStringList &, const QString &)> &done)
{
    const bool ollama = m_provider == Provider::Ollama;
    QNetworkRequest request = jsonRequest(endpoint(ollama ? QStringLiteral("/api/tags") : QStringLiteral("/models")));
    request.setTransferTimeout(s_listTimeoutMs);
    QNetworkReply *reply = m_manager.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ollama, done]() {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = errorFromBody(body);
            done(false, QStringList(), detail.isEmpty() ? reply->errorString() : detail);
            return;
        }
        const QJsonObject object = QJsonDocument::fromJson(body).object();
        QStringList models;
        const QJsonArray entries = object.value(ollama ? QLatin1String("models") : QLatin1String("data")).toArray();
        for (const QJsonValue &entry : entries) {
            const QString name = entry.toObject().value(ollama ? QLatin1String("name") : QLatin1String("id")).toString();
            if (!name.isEmpty()) {
                models << name;
            }
        }
        std::sort(models.begin(), models.end());
        done(true, models, QString());
    });
}

void LlmClient::chat(const LlmRequest &request)
{
    abort();
    m_errorBody.clear();
    m_parser = new ChatStreamParser(m_provider);

    QNetworkRequest networkRequest = jsonRequest(endpoint(m_provider == Provider::Ollama ? QStringLiteral("/api/chat") : QStringLiteral("/chat/completions")));
    networkRequest.setTransferTimeout(s_chatTimeoutMs);
    m_reply = m_manager.post(networkRequest, QJsonDocument(requestBody(m_provider, request)).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &LlmClient::slotReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &LlmClient::slotFinished);
}

void LlmClient::abort()
{
    if (m_reply == nullptr) {
        return;
    }
    // Detach first so nothing from the old reply can reach the state of a request started right after this.
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    delete m_parser;
    m_parser = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void LlmClient::slotReadyRead()
{
    if (m_reply == nullptr || m_parser == nullptr) {
        return;
    }
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400) {
        // An error reply is a plain JSON body, not a stream; keep it whole to report it once the reply is complete.
        m_errorBody += m_reply->readAll();
        return;
    }
    const QString text = m_parser->feed(m_reply->readAll());
    if (!text.isEmpty()) {
        Q_EMIT contentDelta(text);
    }
}

void LlmClient::slotFinished()
{
    QNetworkReply *reply = m_reply;
    ChatStreamParser *parser = m_parser;
    m_reply = nullptr;
    m_parser = nullptr;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray rest = reply->readAll();

    if (status >= 400) {
        m_errorBody += rest;
        const QString detail = errorFromBody(m_errorBody);
        delete parser;
        Q_EMIT failed(detail.isEmpty() ? reply->errorString() : QStringLiteral("%1 (HTTP %2)").arg(detail).arg(status));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QString message = reply->errorString();
        delete parser;
        Q_EMIT failed(message);
        return;
    }
    QString tail = parser->feed(rest);
    tail += parser->finish();
    if (!tail.isEmpty()) {
        Q_EMIT contentDelta(tail);
    }
    const QString error = parser->error();
    const QString content = parser->content();
    const QList<ToolCall> calls = parser->toolCalls();
    delete parser;
    if (!error.isEmpty()) {
        Q_EMIT failed(error);
        return;
    }
    Q_EMIT finished(content, calls);
}

} // namespace AiChat
