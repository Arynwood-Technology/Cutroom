/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#pragma once

#include "aitypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <functional>

namespace AiChat {

enum class Provider {
    /** @brief Ollama's own /api/chat, the default local setup. */
    Ollama,
    /** @brief Anything speaking the OpenAI chat-completions dialect: OpenAI, OpenRouter, LM Studio, llama.cpp, vLLM, Ollama's /v1. */
    OpenAiCompatible,
};

struct LlmRequest
{
    QString model;
    /** @brief The whole conversation to send, system message first. */
    QList<Message> messages;
    /** @brief OpenAI-style function tools; empty to forbid tool use. */
    QJsonArray tools;
    /** @brief Ollama num_ctx, the context window to allocate; 0 keeps the server's default. Ignored by other providers. */
    int contextSize{0};
};

/** @class ChatStreamParser
 *  @brief Turns the byte stream of a streamed chat reply into text and tool calls. Ollama sends one JSON object per line,
 *  OpenAI-style servers send server-sent events whose tool-call arguments arrive in fragments that have to be stitched back together. */
class ChatStreamParser
{
public:
    explicit ChatStreamParser(Provider provider);

    /** @brief Consume more of the stream; returns the reply text that newly became available. */
    QString feed(const QByteArray &chunk);
    /** @brief Signal the end of the stream so a last line without a newline is still read. */
    QString finish();

    QString content() const;
    QList<ToolCall> toolCalls() const;
    /** @brief The error the server reported inside the stream, if any. */
    QString error() const;

private:
    QString parseLine(const QByteArray &line);
    QString parseOllamaLine(const QJsonObject &object);
    QString parseOpenAiPayload(const QJsonObject &object);

    struct PartialCall
    {
        QString id;
        QString name;
        QString arguments;
    };

    Provider m_provider;
    QByteArray m_buffer;
    QString m_content;
    QString m_error;
    QList<ToolCall> m_ollamaCalls;
    QMap<int, PartialCall> m_openAiCalls;
};

/** @class LlmClient
 *  @brief Streams one chat completion at a time from an Ollama or OpenAI-compatible endpoint. */
class LlmClient : public QObject
{
    Q_OBJECT
public:
    explicit LlmClient(QObject *parent = nullptr);
    ~LlmClient() override;

    void setProvider(Provider provider);
    /** @brief Server root for Ollama (http://host:11434), API root for OpenAI-style servers (https://host/v1). */
    void setBaseUrl(const QUrl &url);
    void setApiKey(const QString &key);

    /** @brief Names of the models the endpoint offers. */
    void listModels(const std::function<void(bool ok, const QStringList &models, const QString &error)> &done);
    /** @brief Start a completion; results arrive through the signals. */
    void chat(const LlmRequest &request);
    /** @brief Cancel the running completion without emitting failed(). */
    void abort();
    bool isBusy() const;

    static QJsonObject requestBody(Provider provider, const LlmRequest &request);

Q_SIGNALS:
    /** @brief A piece of reply text arrived. */
    void contentDelta(const QString &text);
    void finished(const QString &content, const QList<AiChat::ToolCall> &toolCalls);
    void failed(const QString &error);

private Q_SLOTS:
    void slotReadyRead();
    void slotFinished();

private:
    QUrl endpoint(const QString &path) const;
    QNetworkRequest jsonRequest(const QUrl &url) const;

    QNetworkAccessManager m_manager;
    Provider m_provider{Provider::Ollama};
    QUrl m_baseUrl;
    QString m_apiKey;
    QNetworkReply *m_reply{nullptr};
    ChatStreamParser *m_parser{nullptr};
    QByteArray m_errorBody;
};

} // namespace AiChat

Q_DECLARE_METATYPE(AiChat::ToolCall)
