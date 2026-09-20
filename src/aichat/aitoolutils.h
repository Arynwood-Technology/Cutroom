/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#pragma once

#include "aitypes.h"

#include <QJsonArray>
#include <QSet>
#include <QStringList>

namespace AiChat {

/** @brief The tool calls a model wrote as plain JSON in its reply text instead of the structured tool_calls field.
 *  Some local models do this, sometimes several objects back to back and sometimes inside a code fence. Only a reply
 *  that starts with such JSON is treated as calls, so prose that merely mentions a tool is never executed. */
QList<ToolCall> toolCallsFromContent(const QString &content);

/** @brief Reduce an MCP tool definition to the OpenAI-style function tool sent to models. Drops outputSchema and every
 *  "title" key, which the model has no use for and which make up about a third of the tool list. */
QJsonObject compactTool(const QJsonObject &mcpTool);

/** @brief Name of a tool in the OpenAI-style function form produced by compactTool(). */
QString toolName(const QJsonObject &tool);

/** @brief Tools ranked by how well their name and description match @p query, best first; tools that don't match at all are
 *  left out. Used to keep the tool list sent to small local models inside their context window. */
QStringList rankTools(const QJsonArray &tools, const QString &query, int limit);

/** @brief One-line "name - first sentence of description" summary of a tool, for the search_tools reply. */
QString briefTool(const QJsonObject &tool);

/** @brief The model to start with among those an Ollama server offers: the first of a short list of families known to call tools
 *  reliably, or else the first one. Ollama lists models alphabetically, so without this the default is often one that cannot call
 *  tools at all (codellama, gemma, tinyllama). */
QString preferredModel(const QStringList &models);

/** @brief Whether a tool's output reports a failure. The mcp-kdenlive tools return errors as ordinary text starting with
 *  "ERROR" rather than setting the MCP isError flag, so the flag alone would show a failed action as a success. */
bool reportsFailure(const QString &output);

/** @brief Whether a tool leaves the content of the project alone: reads, moving the playhead, selecting, and making preview images.
 *  Everything else counts as a change, including undo and redo. */
bool isReadOnlyTool(const QString &name);

/** @brief Tools that can discard unsaved work or write files outside the undo stack, so they need the user's approval. */
bool isRiskyTool(const QString &name);

/** @brief Key of the first string argument that is an unfilled placeholder such as "<clip-id>", or an empty string.
 *  Models emit these instead of asking for a value they don't have. */
QString placeholderArgument(const QJsonObject &arguments);

/** @brief @p text cut to @p maxChars with a note saying how much was dropped. */
QString truncateForModel(const QString &text, int maxChars);

/** @brief Shrink @p history (no system message) to roughly @p budgetChars: old tool outputs are replaced by a short
 *  marker first, then the oldest whole turns are dropped. The latest turn is always kept. */
void trimHistory(QList<Message> &history, int budgetChars);

} // namespace AiChat
