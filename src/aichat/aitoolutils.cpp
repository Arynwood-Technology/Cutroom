/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "aitoolutils.h"

#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace AiChat {

namespace {

const QString s_elidedOutput = QStringLiteral("[older tool output removed to save context]");

/** Words that carry no information about which tool is wanted. */
const QSet<QString> &stopWords()
{
    static const QSet<QString> words = {
        QStringLiteral("the"),    QStringLiteral("and"),   QStringLiteral("for"),  QStringLiteral("with"), QStringLiteral("that"), QStringLiteral("this"),
        QStringLiteral("from"),   QStringLiteral("into"),  QStringLiteral("all"),  QStringLiteral("any"),  QStringLiteral("can"),  QStringLiteral("you"),
        QStringLiteral("please"), QStringLiteral("them"),  QStringLiteral("its"),  QStringLiteral("are"),  QStringLiteral("was"),  QStringLiteral("how"),
        QStringLiteral("what"),   QStringLiteral("which"), QStringLiteral("then"), QStringLiteral("also"),
    };
    return words;
}

/** Words people use for editing operations that the tool names spell differently. */
const QHash<QString, QStringList> &synonyms()
{
    static const QHash<QString, QStringList> map = {
        {QStringLiteral("cut"), {QStringLiteral("split"), QStringLiteral("trim")}},
        {QStringLiteral("split"), {QStringLiteral("cut")}},
        {QStringLiteral("remove"), {QStringLiteral("delete")}},
        {QStringLiteral("delete"), {QStringLiteral("remove")}},
        {QStringLiteral("crossfade"), {QStringLiteral("transition"), QStringLiteral("dissolve")}},
        {QStringLiteral("dissolve"), {QStringLiteral("transition")}},
        {QStringLiteral("louder"), {QStringLiteral("volume")}},
        {QStringLiteral("quieter"), {QStringLiteral("volume")}},
        {QStringLiteral("silence"), {QStringLiteral("mute")}},
        {QStringLiteral("text"), {QStringLiteral("title"), QStringLiteral("subtitle")}},
        {QStringLiteral("caption"), {QStringLiteral("subtitle")}},
        {QStringLiteral("export"), {QStringLiteral("render")}},
        {QStringLiteral("slow"), {QStringLiteral("speed"), QStringLiteral("remap")}},
        {QStringLiteral("fast"), {QStringLiteral("speed"), QStringLiteral("remap")}},
        {QStringLiteral("filter"), {QStringLiteral("effect")}},
        {QStringLiteral("opacity"), {QStringLiteral("transparency")}},
        {QStringLiteral("footage"), {QStringLiteral("clip"), QStringLiteral("media")}},
        {QStringLiteral("video"), {QStringLiteral("clip")}},
        {QStringLiteral("import"), {QStringLiteral("media")}},
        {QStringLiteral("bin"), {QStringLiteral("media"), QStringLiteral("pool")}},
        {QStringLiteral("add"), {QStringLiteral("append"), QStringLiteral("insert")}},
        {QStringLiteral("put"), {QStringLiteral("insert"), QStringLiteral("append")}},
        {QStringLiteral("place"), {QStringLiteral("insert"), QStringLiteral("append")}},
        {QStringLiteral("photo"), {QStringLiteral("image"), QStringLiteral("clip"), QStringLiteral("media")}},
        {QStringLiteral("picture"), {QStringLiteral("image"), QStringLiteral("clip"), QStringLiteral("media")}},
        {QStringLiteral("image"), {QStringLiteral("clip"), QStringLiteral("media")}},
    };
    return map;
}

QString stem(QString word)
{
    static const QStringList suffixes = {QStringLiteral("ing"), QStringLiteral("ed"), QStringLiteral("s")};
    for (const QString &suffix : suffixes) {
        if (word.size() >= suffix.size() + 3 && word.endsWith(suffix)) {
            word.chop(suffix.size());
            // "cutting" -> "cutt" -> "cut"
            if (suffix == QLatin1String("ing") && word.size() > 3 && word.at(word.size() - 1) == word.at(word.size() - 2)) {
                word.chop(1);
            }
            break;
        }
    }
    return word;
}

QStringList tokenize(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[^a-z0-9]+"));
    QStringList tokens;
    const QStringList parts = text.toLower().split(separators, Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part.size() < 3 || stopWords().contains(part)) {
            continue;
        }
        tokens << stem(part);
    }
    return tokens;
}

QString functionField(const QJsonObject &tool, const QString &key)
{
    return tool.value(QLatin1String("function")).toObject().value(key).toString();
}

QJsonObject withoutTitles(const QJsonObject &object)
{
    QJsonObject result;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() == QLatin1String("title") && it.value().isString()) {
            // A property literally named "title" is an object, so only the string-valued schema keyword is dropped.
            continue;
        }
        if (it.value().isObject()) {
            result.insert(it.key(), withoutTitles(it.value().toObject()));
        } else if (it.value().isArray()) {
            QJsonArray items;
            const QJsonArray source = it.value().toArray();
            for (const QJsonValue &item : source) {
                items.append(item.isObject() ? QJsonValue(withoutTitles(item.toObject())) : item);
            }
            result.insert(it.key(), items);
        } else {
            result.insert(it.key(), it.value());
        }
    }
    return result;
}

int messageChars(const Message &message)
{
    int chars = message.content.size();
    for (const ToolCall &call : message.toolCalls) {
        chars += call.name.size() + QJsonDocument(call.arguments).toJson(QJsonDocument::Compact).size();
    }
    return chars;
}

int historyChars(const QList<Message> &history)
{
    int chars = 0;
    for (const Message &message : history) {
        chars += messageChars(message);
    }
    return chars;
}

} // namespace

QList<ToolCall> toolCallsFromContent(const QString &content)
{
    QString text = content.trimmed();
    if (text.startsWith(QLatin1String("```"))) {
        text.remove(QRegularExpression(QStringLiteral("^`+(json)?")));
        text.remove(QRegularExpression(QStringLiteral("`+$")));
        text = text.trimmed();
    }
    QList<ToolCall> calls;
    if (!text.startsWith(QLatin1Char('{'))) {
        return calls;
    }
    // Objects can follow each other with nothing between them, which QJsonDocument rejects as a whole, so peel them off
    // one at a time by growing a candidate up to each closing brace until it parses.
    const QByteArray bytes = text.toUtf8();
    int start = 0;
    while (start < bytes.size()) {
        while (start < bytes.size() && (bytes.at(start) == ' ' || bytes.at(start) == '\n' || bytes.at(start) == '\t' || bytes.at(start) == '\r')) {
            ++start;
        }
        if (start >= bytes.size() || bytes.at(start) != '{') {
            break;
        }
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        int end = -1;
        for (int i = start; i < bytes.size(); ++i) {
            const char c = bytes.at(i);
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
            } else if (c == '"') {
                inString = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}' && --depth == 0) {
                end = i + 1;
                break;
            }
        }
        if (end < 0) {
            break;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(bytes.mid(start, end - start));
        start = end;
        if (!doc.isObject()) {
            break;
        }
        const QJsonObject object = doc.object();
        const QString name = object.value(QLatin1String("name")).toString();
        // llama-style models write "parameters" where others write "arguments".
        const QJsonValue args =
            object.contains(QLatin1String("arguments")) ? object.value(QLatin1String("arguments")) : object.value(QLatin1String("parameters"));
        // An object without an arguments member is just JSON the model chose to answer with, not a call.
        if (name.isEmpty() || !args.isObject()) {
            continue;
        }
        ToolCall call;
        call.name = name;
        call.arguments = args.toObject();
        calls << call;
    }
    return calls;
}

QJsonObject compactTool(const QJsonObject &mcpTool)
{
    QJsonObject function;
    function.insert(QStringLiteral("name"), mcpTool.value(QLatin1String("name")));
    function.insert(QStringLiteral("description"), mcpTool.value(QLatin1String("description")).toString());
    QJsonObject parameters = withoutTitles(mcpTool.value(QLatin1String("inputSchema")).toObject());
    if (parameters.isEmpty()) {
        parameters.insert(QStringLiteral("type"), QStringLiteral("object"));
        parameters.insert(QStringLiteral("properties"), QJsonObject());
    }
    function.insert(QStringLiteral("parameters"), parameters);
    QJsonObject tool;
    tool.insert(QStringLiteral("type"), QStringLiteral("function"));
    tool.insert(QStringLiteral("function"), function);
    return tool;
}

QString toolName(const QJsonObject &tool)
{
    return functionField(tool, QStringLiteral("name"));
}

QStringList rankTools(const QJsonArray &tools, const QString &query, int limit)
{
    QStringList wanted = tokenize(query);
    const QStringList original = wanted;
    for (const QString &word : original) {
        wanted << synonyms().value(word);
    }
    wanted.removeDuplicates();

    struct Entry
    {
        QString name;
        QStringList nameTokens;
        QSet<QString> descriptionTokens;
    };
    QList<Entry> entries;
    // In how many tools each wanted word occurs, in the name or the description.
    QHash<QString, int> documentFrequency;
    for (const QJsonValue &value : tools) {
        const QJsonObject tool = value.toObject();
        Entry entry;
        entry.name = toolName(tool);
        entry.nameTokens = tokenize(QString(entry.name).replace(QLatin1Char('_'), QLatin1Char(' ')));
        const QStringList descriptionList = tokenize(functionField(tool, QStringLiteral("description")));
        entry.descriptionTokens = QSet<QString>(descriptionList.begin(), descriptionList.end());
        for (const QString &word : wanted) {
            if (entry.nameTokens.contains(word) || entry.descriptionTokens.contains(word)) {
                documentFrequency[word]++;
            }
        }
        entries << entry;
    }
    // A word found in many tools says little about which one is wanted ("add" starts a dozen tool names and would otherwise crowd out
    // the tool that actually places a clip); a rare word says a lot.
    QHash<QString, double> weight;
    for (const QString &word : wanted) {
        weight.insert(word, std::log((tools.size() + 1.0) / (documentFrequency.value(word) + 1.0)));
    }

    struct Scored
    {
        double score;
        QString name;
    };
    QList<Scored> scored;
    for (const Entry &entry : entries) {
        double score = 0;
        for (const QString &word : wanted) {
            double match = 0;
            if (entry.nameTokens.contains(word)) {
                match = 4;
            } else {
                for (const QString &nameToken : entry.nameTokens) {
                    if (std::min(word.size(), nameToken.size()) >= 4 && (nameToken.startsWith(word) || word.startsWith(nameToken))) {
                        match = 2;
                        break;
                    }
                }
            }
            if (entry.descriptionTokens.contains(word)) {
                match += 1;
            }
            score += match * weight.value(word);
        }
        if (score > 0) {
            scored.append({score, entry.name});
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) { return a.score > b.score; });
    QStringList names;
    for (const Scored &entry : scored) {
        if (names.size() >= limit) {
            break;
        }
        names << entry.name;
    }
    return names;
}

QString briefTool(const QJsonObject &tool)
{
    const QString description = functionField(tool, QStringLiteral("description")).trimmed();
    int end = description.indexOf(QRegularExpression(QStringLiteral("[.\\n]")));
    QString firstSentence = end < 0 ? description : description.left(end);
    if (firstSentence.size() > 140) {
        firstSentence = firstSentence.left(137) + QStringLiteral("...");
    }
    return QStringLiteral("%1 - %2").arg(toolName(tool), firstSentence);
}

QString preferredModel(const QStringList &models)
{
    // qwen2.5-coder and hermes3 are the ones checked against the mcp-kdenlive tools; the rest are documented tool-calling families.
    // A bare "mistral" is left out on purpose: its older tags cannot call tools.
    static const QStringList families = {
        QStringLiteral("qwen2.5-coder"), QStringLiteral("hermes3"),  QStringLiteral("qwen2.5"),      QStringLiteral("qwen3"),   QStringLiteral("llama3.1"),
        QStringLiteral("llama3.2"),      QStringLiteral("llama3.3"), QStringLiteral("mistral-nemo"), QStringLiteral("gpt-oss"),
    };
    for (const QString &family : families) {
        for (const QString &model : models) {
            if (model.startsWith(family)) {
                return model;
            }
        }
    }
    return models.value(0);
}

bool reportsFailure(const QString &output)
{
    return output.trimmed().startsWith(QLatin1String("ERROR"));
}

bool isReadOnlyTool(const QString &name)
{
    if (name.startsWith(QLatin1String("get_")) || name.startsWith(QLatin1String("find_")) || name.startsWith(QLatin1String("list_"))) {
        return true;
    }
    static const QSet<QString> harmless = {
        // Analysis and previews: they write scratch images or return numbers, not project content.
        QStringLiteral("detect_scenes"),
        QStringLiteral("render_frame"),
        QStringLiteral("render_bin_frame"),
        QStringLiteral("render_contact_sheet"),
        QStringLiteral("render_crop"),
        QStringLiteral("screenshot_window"),
        QStringLiteral("screenshot_panel"),
        QStringLiteral("undo_status"),
        // The playhead and the selection are view state.
        QStringLiteral("seek_to"),
        QStringLiteral("play"),
        QStringLiteral("pause"),
        QStringLiteral("set_playback_speed"),
        QStringLiteral("go_to_next_edit"),
        QStringLiteral("go_to_previous_edit"),
        QStringLiteral("go_to_next_marker"),
        QStringLiteral("go_to_previous_marker"),
        QStringLiteral("set_selection"),
        QStringLiteral("add_to_selection"),
        QStringLiteral("clear_selection"),
        QStringLiteral("select_all"),
        QStringLiteral("select_current_track"),
        QStringLiteral("select_items_in_range"),
    };
    return harmless.contains(name);
}

bool isRiskyTool(const QString &name)
{
    static const QSet<QString> risky = {
        QStringLiteral("new_project"),  QStringLiteral("open_project"),     QStringLiteral("load_project"),     QStringLiteral("checkpoint_restore"),
        QStringLiteral("render_video"), QStringLiteral("export_subtitles"), QStringLiteral("abort_render_job"),
    };
    return risky.contains(name);
}

QString placeholderArgument(const QJsonObject &arguments)
{
    static const QRegularExpression placeholder(QStringLiteral("^\\s*<[^<>]+>\\s*$"));
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (it.value().isString() && placeholder.match(it.value().toString()).hasMatch()) {
            return it.key();
        }
    }
    return {};
}

QString truncateForModel(const QString &text, int maxChars)
{
    if (text.size() <= maxChars) {
        return text;
    }
    return text.left(maxChars) + QStringLiteral("\n[truncated: %1 more characters]").arg(text.size() - maxChars);
}

void trimHistory(QList<Message> &history, int budgetChars)
{
    if (historyChars(history) <= budgetChars) {
        return;
    }
    int latestTurn = 0;
    for (int i = 0; i < history.size(); ++i) {
        if (history.at(i).role == Message::Role::User) {
            latestTurn = i;
        }
    }
    // The tool outputs of the latest turn are what the model is working from right now, so they are never elided.
    for (int i = 0; i < latestTurn; ++i) {
        Message &message = history[i];
        if (message.role == Message::Role::Tool && message.content != s_elidedOutput) {
            message.content = s_elidedOutput;
            if (historyChars(history) <= budgetChars) {
                return;
            }
        }
    }
    while (historyChars(history) > budgetChars) {
        int nextTurn = -1;
        int seenUsers = 0;
        for (int i = 0; i < history.size(); ++i) {
            if (history.at(i).role == Message::Role::User && ++seenUsers == 2) {
                nextTurn = i;
                break;
            }
        }
        if (nextTurn < 0) {
            // Only the latest turn is left; it has to stay even if it alone is over budget.
            return;
        }
        history.erase(history.begin(), history.begin() + nextTurn);
    }
}

} // namespace AiChat
