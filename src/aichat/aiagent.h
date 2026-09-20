/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#pragma once

#include "aillmclient.h"
#include "aimcpclient.h"

#include <QHash>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QUrl>

namespace AiChat {

/** @brief When the assistant has to ask before running a tool. */
enum class ApprovalMode {
    /** @brief Only for tools that can lose unsaved work or overwrite files. */
    RiskyOnly = 0,
    /** @brief For every tool that changes the project. Reading and looking around never asks. */
    EveryChange = 1,
    /** @brief Never; the assistant runs whatever the model asks for. */
    Never = 2,
};

struct AgentSettings
{
    Provider provider{Provider::Ollama};
    /** @brief Root of the Ollama server or API root of the OpenAI-compatible endpoint. */
    QUrl llmUrl;
    QString apiKey;
    QString model;
    /** @brief The mcp-kdenlive streamable-HTTP endpoint. */
    QUrl mcpUrl;
    /** @brief Context window requested from Ollama, in tokens. */
    int contextSize{16384};
    /** @brief How many rounds of tool calls one request may take before the model is made to answer. */
    int maxToolRounds{8};
    /** @brief Send only the tools that fit the request instead of all of them. Needed for small context windows. */
    bool relevantToolsOnly{true};
    /** @brief Small local models sometimes flail, so by default nothing that changes the project runs without a yes. */
    ApprovalMode approval{ApprovalMode::EveryChange};
};

/** @class Agent
 *  @brief Runs the conversation: sends it to the model, runs the tools the model asks for through the MCP server, feeds
 *  the results back, and repeats until the model answers in plain text. All work is asynchronous; the UI only listens
 *  to the signals. */
class Agent : public QObject
{
    Q_OBJECT
public:
    explicit Agent(QObject *parent = nullptr);

    void setSettings(const AgentSettings &settings);
    AgentSettings settings() const;
    /** @brief Endpoint access for the panel's connection checks. */
    LlmClient *llm();
    /** @brief Re-read the tool list from the server; @p done tells how many tools were found or why not. */
    void refreshTools(const std::function<void(bool ok, int count, const QString &error)> &done);

    /** @brief Add @p text to the conversation and start working on it. Ignored while busy. */
    void send(const QString &text);
    /** @brief Cancel the running request. The conversation stays valid for the next message. */
    void stop();
    /** @brief Forget the conversation. */
    void reset();
    /** @brief Answer the pending approvalRequested(). */
    void resolveApproval(bool allow);
    bool isBusy() const;

    /** @brief The instructions every conversation starts with. */
    static QString systemPrompt(bool relevantToolsOnly);

Q_SIGNALS:
    void busyChanged(bool busy);
    /** @brief A new reply from the model begins; assistantDelta() and assistantFinished() belong to it. */
    void assistantStarted();
    void assistantDelta(const QString &text);
    void assistantFinished();
    void toolStarted(const QString &name, const QJsonObject &arguments);
    void toolFinished(const QString &name, bool ok, const QString &result);
    void approvalRequested(const QString &name, const QJsonObject &arguments);
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void slotDelta(const QString &text);
    void slotLlmFinished(const QString &content, const QList<AiChat::ToolCall> &toolCalls);
    void slotLlmFailed(const QString &error);

private:
    void ensureTools(const std::function<void(bool ok, const QString &error)> &then);
    void beginRound();
    void runNextCall();
    void executeCall(const ToolCall &call);
    void finishCall(const ToolCall &call, bool ok, const QString &result);
    QString searchTools(const QString &query);
    QJsonArray toolsForRequest() const;
    QString recentUserText() const;
    void setBusy(bool busy);
    void endTurn();
    void answerPendingCalls(const QString &reason);

    McpClient m_mcp;
    LlmClient m_llm;
    AgentSettings m_settings;
    QJsonArray m_allTools;
    QSet<QString> m_toolNames;
    /** @brief Tools used or found lately, most recent first; they stay on offer when only relevant tools are sent. */
    QStringList m_recentTools;
    QList<Message> m_history;
    /** @brief Calls of the latest assistant message that have not been started yet. */
    QList<ToolCall> m_pendingCalls;
    /** @brief The call being approved or run. Every call of an assistant message must end up with a result, or the next
     *  request is malformed for OpenAI-style servers, so stop() answers this one too. */
    ToolCall m_currentCall;
    bool m_hasCurrentCall{false};
    bool m_awaitingApproval{false};
    QPointer<QNetworkReply> m_toolReply;

    // State of the reply being streamed.
    QString m_streamText;
    bool m_streamStarted{false};
    bool m_streamHeld{false};

    /** @brief How often each project-changing call (name and arguments) was made in this request. */
    QHash<QString, int> m_changeCounts;
    int m_requestCalls{0};
    int m_round{0};
    bool m_finalRound{false};
    int m_callCounter{0};
    /** @brief Bumped by stop() so that callbacks of a cancelled request can tell they are stale. */
    int m_generation{0};
    bool m_busy{false};
};

} // namespace AiChat
