/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "aiagent.h"
#include "aitoolutils.h"

#include <KLocalizedString>

#include <QJsonDocument>
#include <QNetworkReply>

#include <algorithm>

namespace AiChat {

namespace {

// A tool output can be a whole timeline; past this the model gains little and the context window loses a lot.
constexpr int s_maxToolResultChars = 6000;
constexpr int s_relevantToolLimit = 24;
constexpr int s_recentToolLimit = 16;
constexpr int s_defaultContextSize = 8192;
// Small local models sometimes flail: one checked model made about 70 edits in a single request, deleting and moving clips it had
// just placed. These bound what a runaway can do even when every change is allowed.
constexpr int s_maxCallsPerReply = 12;
constexpr int s_maxCallsPerRequest = 40;
constexpr int s_maxIdenticalChanges = 2;
const QString s_searchToolName = QStringLiteral("search_tools");

/** Tools kept on offer whatever the request says: the ones to look at the project and take back what was done, and the basic clip
 *  operations, so that the everyday edits never depend on a keyword match. */
const QStringList &coreTools()
{
    static const QStringList tools = {
        QStringLiteral("get_timeline_summary"),
        QStringLiteral("get_project_info"),
        QStringLiteral("find_clip"),
        QStringLiteral("get_media_pool"),
        QStringLiteral("get_track_list"),
        QStringLiteral("get_position"),
        QStringLiteral("seek_to"),
        QStringLiteral("undo"),
        QStringLiteral("redo"),
        QStringLiteral("append_clips"),
        QStringLiteral("insert_clip"),
        QStringLiteral("move_clip"),
        QStringLiteral("delete_clip"),
        QStringLiteral("split_clip"),
    };
    return tools;
}

QJsonObject searchToolDefinition()
{
    QJsonObject query;
    query.insert(QStringLiteral("type"), QStringLiteral("string"));
    query.insert(QStringLiteral("description"), QStringLiteral("What you want to do, in a few words, e.g. 'fade out audio'"));
    QJsonObject properties;
    properties.insert(QStringLiteral("query"), query);
    QJsonObject parameters;
    parameters.insert(QStringLiteral("type"), QStringLiteral("object"));
    parameters.insert(QStringLiteral("properties"), properties);
    parameters.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("query")});
    QJsonObject function;
    function.insert(QStringLiteral("name"), s_searchToolName);
    function.insert(
        QStringLiteral("description"),
        QStringLiteral("Find editor tools by keyword when none of your current tools fits the request. The tools it returns can be called on your next step."));
    function.insert(QStringLiteral("parameters"), parameters);
    QJsonObject tool;
    tool.insert(QStringLiteral("type"), QStringLiteral("function"));
    tool.insert(QStringLiteral("function"), function);
    return tool;
}

} // namespace

Agent::Agent(QObject *parent)
    : QObject(parent)
{
    connect(&m_llm, &LlmClient::contentDelta, this, &Agent::slotDelta);
    connect(&m_llm, &LlmClient::finished, this, &Agent::slotLlmFinished);
    connect(&m_llm, &LlmClient::failed, this, &Agent::slotLlmFailed);
}

void Agent::setSettings(const AgentSettings &settings)
{
    const bool toolServerChanged = settings.mcpUrl != m_settings.mcpUrl;
    m_settings = settings;
    m_llm.setProvider(settings.provider);
    m_llm.setBaseUrl(settings.llmUrl);
    m_llm.setApiKey(settings.apiKey);
    m_mcp.setUrl(settings.mcpUrl);
    if (toolServerChanged) {
        m_allTools = QJsonArray();
        m_toolNames.clear();
    }
}

AgentSettings Agent::settings() const
{
    return m_settings;
}

LlmClient *Agent::llm()
{
    return &m_llm;
}

bool Agent::isBusy() const
{
    return m_busy;
}

void Agent::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged(busy);
    }
}

void Agent::refreshTools(const std::function<void(bool, int, const QString &)> &done)
{
    m_mcp.listTools([this, done](bool ok, const QJsonArray &tools, const QString &error) {
        if (!ok) {
            done(false, 0, error);
            return;
        }
        m_allTools = QJsonArray();
        m_toolNames.clear();
        for (const QJsonValue &value : tools) {
            const QJsonObject tool = compactTool(value.toObject());
            m_allTools.append(tool);
            m_toolNames.insert(toolName(tool));
        }
        done(true, int(m_allTools.size()), QString());
    });
}

void Agent::ensureTools(const std::function<void(bool, const QString &)> &then)
{
    if (!m_allTools.isEmpty()) {
        then(true, QString());
        return;
    }
    refreshTools([then](bool ok, int, const QString &error) { then(ok, error); });
}

QString Agent::systemPrompt(bool relevantToolsOnly)
{
    QString prompt = QStringLiteral(
        "You are the Cutroom assistant, built into Arynwood Cutroom, a video editor based on Kdenlive. "
        "You work on the project the user has open by calling tools. Call a tool to take an action or inspect state; don't just describe what you would do. "
        "Prefer one tool that does a whole job over many small calls. "
        "Clips that are already in the project's bin (media pool) are used by their bin_id: call get_media_pool for the ids (photos and other still images "
        "have "
        "type 5), get_track_list for a track id, then append_clips to put several clips in order on one track, or insert_clip for a single clip. "
        "build_timeline is only for importing a folder of video files from disk that are not in the project yet; never use it for clips already in the bin. "
        "Positions and durations you pass to tools are frame numbers, not seconds; when the user speaks in seconds, convert with the project's frame rate "
        "(get_project_info) unless a tool's description says otherwise. "
        "Once a tool result gives you enough information to answer the user's question, or once an action succeeds, stop calling tools and reply in plain "
        "text. "
        "Do not repeat a call that already gave you the answer, and do not keep probing for data that a tool already told you doesn't exist.\n\n"
        "Every tool you can call is in the tools list you were given; there are no others. Do not guess a plausible-sounding name and call it anyway. "
        "If nothing in your tool list does what's needed, say so in plain text instead of inventing one. "
        "If you only know a clip by its filename, call find_clip first to get its real id, because every other clip tool needs the id, not a name. "
        "Never put a placeholder like '<clip-id>' in a tool argument. If you don't have a real value for a required argument, "
        "say what's missing in plain text instead of calling the tool. "
        "If a call fails, read the error and try different arguments or a different tool before giving up; ask the user only for information no tool can give "
        "you.\n"
        "Answering a question is not a reason to then change something: if the user asked what a value is and a tool already told you, "
        "report it and stop. There is an undo tool, but not every action can be undone, so say plainly what you changed.");
    if (relevantToolsOnly) {
        prompt += QStringLiteral("\n\nYou are shown only the tools that look most relevant to the request, not all of them. If none of them fits, "
                                 "call search_tools with a short description of what you want to do; the tools it finds become available on your next step.");
    }
    return prompt;
}

QString Agent::recentUserText() const
{
    QStringList texts;
    for (int i = m_history.size() - 1; i >= 0 && texts.size() < 2; --i) {
        if (m_history.at(i).role == Message::Role::User) {
            texts << m_history.at(i).content;
        }
    }
    return texts.join(QLatin1Char(' '));
}

QJsonArray Agent::toolsForRequest() const
{
    if (!m_settings.relevantToolsOnly) {
        return m_allTools;
    }
    QSet<QString> wanted;
    for (const QString &name : coreTools()) {
        wanted.insert(name);
    }
    for (const QString &name : m_recentTools) {
        wanted.insert(name);
    }
    const QStringList ranked = rankTools(m_allTools, recentUserText(), s_relevantToolLimit);
    for (const QString &name : ranked) {
        wanted.insert(name);
    }
    QJsonArray tools;
    for (const QJsonValue &value : m_allTools) {
        if (wanted.contains(toolName(value.toObject()))) {
            tools.append(value);
        }
    }
    tools.append(searchToolDefinition());
    return tools;
}

void Agent::send(const QString &text)
{
    if (m_busy || text.trimmed().isEmpty()) {
        return;
    }
    Message user;
    user.role = Message::Role::User;
    user.content = text.trimmed();
    m_history << user;
    m_round = 0;
    m_finalRound = false;
    m_requestCalls = 0;
    m_changeCounts.clear();
    setBusy(true);

    const int generation = m_generation;
    ensureTools([this, generation](bool ok, const QString &error) {
        if (generation != m_generation) {
            return;
        }
        if (!ok) {
            // Nothing will answer this message, so don't leave it in the conversation to pair up with the retry.
            m_history.removeLast();
            Q_EMIT errorOccurred(i18n("%1\n\nStart the mcp-kdenlive service (see mcp-kdenlive/README.md), then try again.", error));
            endTurn();
            return;
        }
        beginRound();
    });
}

void Agent::beginRound()
{
    m_finalRound = m_round >= m_settings.maxToolRounds;
    m_streamText.clear();
    m_streamStarted = false;
    m_streamHeld = false;

    LlmRequest request;
    request.model = m_settings.model;
    request.contextSize = m_settings.contextSize;
    if (!m_finalRound) {
        request.tools = toolsForRequest();
    }

    Message system;
    system.role = Message::Role::System;
    system.content = systemPrompt(m_settings.relevantToolsOnly);

    // Roughly three characters to the token leaves room for the model's own reply.
    const int contextSize = m_settings.contextSize > 0 ? m_settings.contextSize : s_defaultContextSize;
    const int overhead = int(QJsonDocument(request.tools).toJson(QJsonDocument::Compact).size()) + int(system.content.size());
    trimHistory(m_history, std::max(4000, contextSize * 3 - overhead));

    request.messages << system;
    request.messages += m_history;
    if (m_finalRound) {
        Message stop;
        stop.role = Message::Role::User;
        stop.content = QStringLiteral("Stop calling tools. Answer now, in plain text, using only what the tool results above already told you.");
        request.messages << stop;
    }
    m_llm.chat(request);
}

void Agent::slotDelta(const QString &text)
{
    m_streamText += text;
    if (m_streamStarted) {
        Q_EMIT assistantDelta(text);
        return;
    }
    if (m_streamHeld) {
        return;
    }
    const QString trimmed = m_streamText.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    // A reply that opens like JSON may be a tool call the model wrote as text; hold it back until it is known whether it is.
    if (trimmed.startsWith(QLatin1Char('{')) || trimmed.startsWith(QLatin1String("```")) || QStringLiteral("```").startsWith(trimmed)) {
        m_streamHeld = true;
        return;
    }
    m_streamStarted = true;
    Q_EMIT assistantStarted();
    Q_EMIT assistantDelta(m_streamText);
}

void Agent::slotLlmFinished(const QString &rawContent, const QList<ToolCall> &nativeCalls)
{
    QString content = rawContent;
    QList<ToolCall> calls = nativeCalls;
    bool callsFromContent = false;
    if (calls.isEmpty()) {
        calls = toolCallsFromContent(content);
        callsFromContent = !calls.isEmpty();
    }
    if (m_finalRound) {
        // This request offered no tools; whatever the model still writes is its answer.
        calls.clear();
        callsFromContent = false;
    }
    // Local models occasionally emit a call with no name; it can't be run and would poison the conversation.
    calls.erase(std::remove_if(calls.begin(), calls.end(), [](const ToolCall &call) { return call.name.isEmpty(); }), calls.end());
    if (calls.size() > s_maxCallsPerReply) {
        Q_EMIT errorOccurred(i18n("The model asked for %1 actions at once; only the first %2 were run.", calls.size(), s_maxCallsPerReply));
        calls = calls.mid(0, s_maxCallsPerReply);
    }
    for (ToolCall &call : calls) {
        if (call.id.isEmpty()) {
            call.id = QStringLiteral("call_%1").arg(++m_callCounter);
        }
    }

    if (callsFromContent) {
        content.clear();
    } else if (m_streamHeld && !m_streamStarted && !content.trimmed().isEmpty()) {
        m_streamStarted = true;
        Q_EMIT assistantStarted();
        Q_EMIT assistantDelta(content);
    }
    if (m_streamStarted) {
        Q_EMIT assistantFinished();
    }
    m_streamStarted = false;
    m_streamHeld = false;

    if (calls.isEmpty()) {
        if (content.trimmed().isEmpty()) {
            Q_EMIT errorOccurred(i18n("The model returned an empty reply. Try again, or pick a different model."));
        } else {
            Message reply;
            reply.role = Message::Role::Assistant;
            reply.content = content;
            m_history << reply;
        }
        endTurn();
        return;
    }
    Message reply;
    reply.role = Message::Role::Assistant;
    reply.content = content;
    reply.toolCalls = calls;
    m_history << reply;
    m_pendingCalls = calls;
    runNextCall();
}

void Agent::slotLlmFailed(const QString &error)
{
    if (m_streamStarted) {
        Q_EMIT assistantFinished();
    }
    m_streamStarted = false;
    m_streamHeld = false;
    Q_EMIT errorOccurred(i18n("No reply from %1: %2", m_settings.llmUrl.toString(), error));
    endTurn();
}

void Agent::runNextCall()
{
    if (m_pendingCalls.isEmpty()) {
        ++m_round;
        beginRound();
        return;
    }
    m_currentCall = m_pendingCalls.takeFirst();
    m_hasCurrentCall = true;
    const ToolCall call = m_currentCall;

    if (++m_requestCalls > s_maxCallsPerRequest) {
        Q_EMIT toolStarted(call.name, call.arguments);
        // Whatever else is queued is refused the same way, and the next round is the last: no tools, just an answer.
        m_round = m_settings.maxToolRounds;
        finishCall(call, false, QStringLiteral("Too many tool calls for one request. Stop calling tools and tell the user how far you got."));
        return;
    }
    if (call.name == s_searchToolName) {
        Q_EMIT toolStarted(call.name, call.arguments);
        finishCall(call, true, searchTools(call.arguments.value(QLatin1String("query")).toString()));
        return;
    }
    if (!m_toolNames.contains(call.name)) {
        Q_EMIT toolStarted(call.name, call.arguments);
        QString message = QStringLiteral("Unknown tool '%1'. Only call tools from your tool list; don't invent names.").arg(call.name);
        QString lookalike = call.name;
        const QStringList similar = rankTools(m_allTools, lookalike.replace(QLatin1Char('_'), QLatin1Char(' ')), 3);
        if (!similar.isEmpty()) {
            message += QStringLiteral(" Did you mean: %1?").arg(similar.join(QStringLiteral(", ")));
        }
        finishCall(call, false, message);
        return;
    }
    const QString placeholder = placeholderArgument(call.arguments);
    if (!placeholder.isEmpty()) {
        Q_EMIT toolStarted(call.name, call.arguments);
        finishCall(
            call, false,
            QStringLiteral(
                "Argument '%1' is a placeholder, not a real value. Look the real value up with another tool, or ask the user for it, instead of guessing.")
                .arg(placeholder));
        return;
    }
    // Looking at the project can repeat legitimately, since it may have changed in between; changing it the same way twice cannot.
    if (!isReadOnlyTool(call.name)) {
        const QString key = call.name + QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact));
        if (++m_changeCounts[key] > s_maxIdenticalChanges) {
            Q_EMIT toolStarted(call.name, call.arguments);
            finishCall(call, false,
                       QStringLiteral("You already made this exact change %1 times, so it will not be run again. Take a different approach, or tell the user "
                                      "what is blocking you.")
                           .arg(s_maxIdenticalChanges));
            return;
        }
    }
    const bool needsApproval = m_settings.approval == ApprovalMode::EveryChange ? !isReadOnlyTool(call.name)
                               : m_settings.approval == ApprovalMode::RiskyOnly ? isRiskyTool(call.name)
                                                                                : false;
    if (needsApproval) {
        m_awaitingApproval = true;
        Q_EMIT approvalRequested(call.name, call.arguments);
        return;
    }
    executeCall(call);
}

void Agent::resolveApproval(bool allow)
{
    if (!m_awaitingApproval) {
        return;
    }
    m_awaitingApproval = false;
    const ToolCall call = m_currentCall;
    if (allow) {
        executeCall(call);
        return;
    }
    Q_EMIT toolStarted(call.name, call.arguments);
    finishCall(call, false, QStringLiteral("The user declined to run this action. Don't retry it; ask what they would like instead."));
}

void Agent::executeCall(const ToolCall &call)
{
    Q_EMIT toolStarted(call.name, call.arguments);
    const int generation = m_generation;
    m_toolReply = m_mcp.callTool(call.name, call.arguments, [this, call, generation](bool ok, const QString &text) {
        if (generation != m_generation) {
            return;
        }
        m_toolReply.clear();
        finishCall(call, ok, text);
    });
}

void Agent::finishCall(const ToolCall &call, bool ok, const QString &result)
{
    m_hasCurrentCall = false;
    if (m_toolNames.contains(call.name)) {
        m_recentTools.removeAll(call.name);
        m_recentTools.prepend(call.name);
        while (m_recentTools.size() > s_recentToolLimit) {
            m_recentTools.removeLast();
        }
    }
    Message message;
    message.role = Message::Role::Tool;
    message.toolCallId = call.id;
    message.toolName = call.name;
    // Failures reported as text already say ERROR; only the ones the client itself raised still need the prefix.
    message.content = truncateForModel(ok || reportsFailure(result) ? result : QStringLiteral("ERROR: %1").arg(result), s_maxToolResultChars);
    m_history << message;
    Q_EMIT toolFinished(call.name, ok, result);
    runNextCall();
}

QString Agent::searchTools(const QString &query)
{
    const QStringList names = rankTools(m_allTools, query, 8);
    if (names.isEmpty()) {
        return QStringLiteral("No tools match that description. Try different keywords.");
    }
    QStringList lines;
    for (const QString &name : names) {
        for (const QJsonValue &value : m_allTools) {
            if (toolName(value.toObject()) == name) {
                lines << briefTool(value.toObject());
                break;
            }
        }
        m_recentTools.removeAll(name);
        m_recentTools.prepend(name);
    }
    while (m_recentTools.size() > s_recentToolLimit) {
        m_recentTools.removeLast();
    }
    return QStringLiteral("Matching tools, now available to call:\n%1").arg(lines.join(QLatin1Char('\n')));
}

void Agent::answerPendingCalls(const QString &reason)
{
    auto answer = [this, &reason](const ToolCall &call) {
        Message message;
        message.role = Message::Role::Tool;
        message.toolCallId = call.id;
        message.toolName = call.name;
        message.content = reason;
        m_history << message;
    };
    if (m_hasCurrentCall) {
        answer(m_currentCall);
        m_hasCurrentCall = false;
    }
    for (const ToolCall &call : m_pendingCalls) {
        answer(call);
    }
    m_pendingCalls.clear();
}

void Agent::stop()
{
    if (!m_busy) {
        return;
    }
    ++m_generation;
    m_llm.abort();
    if (m_toolReply) {
        m_toolReply->abort();
    }
    m_toolReply.clear();
    m_awaitingApproval = false;
    if (m_streamStarted) {
        Q_EMIT assistantFinished();
    }
    m_streamStarted = false;
    m_streamHeld = false;
    answerPendingCalls(QStringLiteral("Cancelled by the user."));
    setBusy(false);
}

void Agent::reset()
{
    stop();
    m_history.clear();
    m_recentTools.clear();
}

void Agent::endTurn()
{
    setBusy(false);
}

} // namespace AiChat
