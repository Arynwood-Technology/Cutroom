/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

/** @namespace AiChat
 *  @brief The Cutroom assistant: a chat panel that drives the editor through the mcp-kdenlive tool server
 *  using a local Ollama model or any OpenAI-compatible endpoint. */
namespace AiChat {

struct ToolCall
{
    QString id;
    QString name;
    QJsonObject arguments;
};

/** @brief One entry of the conversation, independent of which LLM provider it will be serialized for. */
struct Message
{
    enum class Role { System, User, Assistant, Tool };
    Role role{Role::User};
    QString content;
    /** @brief Assistant messages only: the tools the model asked to run. */
    QList<ToolCall> toolCalls;
    /** @brief Tool messages only: which call this is the result of. */
    QString toolCallId;
    QString toolName;
};

} // namespace AiChat
