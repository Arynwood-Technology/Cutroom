/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "catch.hpp"

#include "aichat/aillmclient.h"
#include "aichat/aimcpclient.h"
#include "aichat/aitoolutils.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace AiChat;

namespace {

QJsonObject mcpTool(const QString &name, const QString &description)
{
    QJsonObject property;
    property.insert(QStringLiteral("type"), QStringLiteral("string"));
    property.insert(QStringLiteral("title"), QStringLiteral("Clip Id"));
    QJsonObject properties;
    properties.insert(QStringLiteral("clip_id"), property);
    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("title"), QStringLiteral("Arguments"));
    schema.insert(QStringLiteral("properties"), properties);
    QJsonObject tool;
    tool.insert(QStringLiteral("name"), name);
    tool.insert(QStringLiteral("description"), description);
    tool.insert(QStringLiteral("inputSchema"), schema);
    tool.insert(QStringLiteral("outputSchema"), QJsonObject());
    return tool;
}

QJsonArray toolset()
{
    const QList<QPair<QString, QString>> defs = {
        {QStringLiteral("split_clip"), QStringLiteral("Split a clip in two at a frame.")},
        {QStringLiteral("cut_clips"), QStringLiteral("Cut all clips on a track at the given position.")},
        {QStringLiteral("set_audio_fade"), QStringLiteral("Set the fade in or fade out of a clip's audio.")},
        {QStringLiteral("add_transition"), QStringLiteral("Add a cross-dissolve transition between two clips.")},
        {QStringLiteral("get_project_info"), QStringLiteral("Get current project settings: name, fps, resolution.")},
        {QStringLiteral("add_marker"), QStringLiteral("Add a timeline marker with a comment.")},
        {QStringLiteral("render_video"), QStringLiteral("Export the timeline to a video file.")},
    };
    QJsonArray tools;
    for (const auto &def : defs) {
        tools.append(compactTool(mcpTool(def.first, def.second)));
    }
    return tools;
}

QString sseBody(const QString &json)
{
    return QStringLiteral("event: message\r\ndata: %1\r\n\r\n").arg(json);
}

} // namespace

TEST_CASE("Tool calls written as text are recovered", "[AiChat]")
{
    SECTION("A single call")
    {
        const QList<ToolCall> calls = toolCallsFromContent(QStringLiteral("{\"name\": \"find_clip\", \"arguments\": {\"name\": \"intro.mp4\"}}"));
        REQUIRE(calls.size() == 1);
        CHECK(calls.at(0).name == QLatin1String("find_clip"));
        CHECK(calls.at(0).arguments.value(QLatin1String("name")).toString() == QLatin1String("intro.mp4"));
    }
    SECTION("Several calls back to back, one inside a code fence")
    {
        const QString text = QStringLiteral("```json\n{\"name\":\"a\",\"arguments\":{}}\n{\"name\":\"b\",\"arguments\":{\"x\":1}}\n```");
        const QList<ToolCall> calls = toolCallsFromContent(text);
        REQUIRE(calls.size() == 2);
        CHECK(calls.at(0).name == QLatin1String("a"));
        CHECK(calls.at(1).name == QLatin1String("b"));
        CHECK(calls.at(1).arguments.value(QLatin1String("x")).toInt() == 1);
    }
    SECTION("Braces inside string values do not end an object early")
    {
        const QList<ToolCall> calls = toolCallsFromContent(QStringLiteral("{\"name\":\"add_title\",\"arguments\":{\"text\":\"a } b { c\"}}"));
        REQUIRE(calls.size() == 1);
        CHECK(calls.at(0).arguments.value(QLatin1String("text")).toString() == QLatin1String("a } b { c"));
    }
    SECTION("The parameters spelling is accepted")
    {
        const QList<ToolCall> calls = toolCallsFromContent(QStringLiteral("{\"name\":\"undo\",\"parameters\":{}}"));
        REQUIRE(calls.size() == 1);
        CHECK(calls.at(0).name == QLatin1String("undo"));
    }
    SECTION("Prose, or JSON that is not a call, is left alone")
    {
        CHECK(toolCallsFromContent(QStringLiteral("I will call {\"name\":\"undo\",\"arguments\":{}} now")).isEmpty());
        CHECK(toolCallsFromContent(QStringLiteral("{\"name\": \"Alice\"}")).isEmpty());
        CHECK(toolCallsFromContent(QString()).isEmpty());
    }
}

TEST_CASE("Tool definitions are compacted for the model", "[AiChat]")
{
    const QJsonObject tool = compactTool(mcpTool(QStringLiteral("split_clip"), QStringLiteral("Split a clip.")));
    const QJsonObject function = tool.value(QLatin1String("function")).toObject();
    CHECK(tool.value(QLatin1String("type")).toString() == QLatin1String("function"));
    CHECK(function.value(QLatin1String("name")).toString() == QLatin1String("split_clip"));
    const QJsonObject parameters = function.value(QLatin1String("parameters")).toObject();
    CHECK_FALSE(parameters.contains(QLatin1String("title")));
    CHECK_FALSE(parameters.value(QLatin1String("properties")).toObject().value(QLatin1String("clip_id")).toObject().contains(QLatin1String("title")));
    CHECK(parameters.value(QLatin1String("properties")).toObject().value(QLatin1String("clip_id")).toObject().value(QLatin1String("type")).toString() ==
          QLatin1String("string"));
    CHECK_FALSE(tool.contains(QLatin1String("outputSchema")));

    SECTION("A parameter that is itself called title survives")
    {
        QJsonObject titleParam;
        titleParam.insert(QStringLiteral("type"), QStringLiteral("string"));
        titleParam.insert(QStringLiteral("title"), QStringLiteral("Title"));
        QJsonObject properties;
        properties.insert(QStringLiteral("title"), titleParam);
        QJsonObject schema;
        schema.insert(QStringLiteral("properties"), properties);
        QJsonObject source;
        source.insert(QStringLiteral("name"), QStringLiteral("add_title"));
        source.insert(QStringLiteral("inputSchema"), schema);
        const QJsonObject kept = compactTool(source).value(QLatin1String("function")).toObject().value(QLatin1String("parameters")).toObject();
        const QJsonObject keptTitle = kept.value(QLatin1String("properties")).toObject().value(QLatin1String("title")).toObject();
        CHECK(keptTitle.value(QLatin1String("type")).toString() == QLatin1String("string"));
        CHECK_FALSE(keptTitle.contains(QLatin1String("title")));
    }
    SECTION("A tool without parameters still gets an object schema")
    {
        QJsonObject source;
        source.insert(QStringLiteral("name"), QStringLiteral("undo"));
        const QJsonObject bare = compactTool(source).value(QLatin1String("function")).toObject().value(QLatin1String("parameters")).toObject();
        CHECK(bare.value(QLatin1String("type")).toString() == QLatin1String("object"));
    }
}

TEST_CASE("Tools are ranked by the request", "[AiChat]")
{
    const QJsonArray tools = toolset();

    SECTION("Editing words find the matching tools first")
    {
        const QStringList ranked = rankTools(tools, QStringLiteral("cut the clip at 5 seconds"), 3);
        REQUIRE(!ranked.isEmpty());
        CHECK((ranked.at(0) == QLatin1String("cut_clips") || ranked.at(0) == QLatin1String("split_clip")));
        CHECK(ranked.contains(QLatin1String("split_clip")));
        CHECK(ranked.contains(QLatin1String("cut_clips")));
    }
    SECTION("Inflected words and synonyms match")
    {
        CHECK(rankTools(tools, QStringLiteral("fading the audio out"), 1).value(0) == QLatin1String("set_audio_fade"));
        CHECK(rankTools(tools, QStringLiteral("add a crossfade"), 1).value(0) == QLatin1String("add_transition"));
        CHECK(rankTools(tools, QStringLiteral("export it"), 1).value(0) == QLatin1String("render_video"));
    }
    SECTION("The limit is respected and nonsense matches nothing")
    {
        CHECK(rankTools(tools, QStringLiteral("clip clips clip"), 2).size() <= 2);
        CHECK(rankTools(tools, QStringLiteral("zzzz qqqq"), 5).isEmpty());
        CHECK(rankTools(tools, QString(), 5).isEmpty());
    }
    SECTION("A tool has a one-line summary")
    {
        CHECK(briefTool(tools.at(0).toObject()) == QLatin1String("split_clip - Split a clip in two at a frame"));
    }
}

TEST_CASE("A request that starts with 'add' still finds the tools that place clips", "[AiChat]")
{
    // A real failure: "add" starts a dozen tool names, and used to crowd out append_clips and insert_clip so the model was never
    // shown them. The descriptions of the last four are the real ones from mcp-kdenlive.
    const QList<QPair<QString, QString>> defs = {
        {QStringLiteral("add_clip_marker"), QStringLiteral("Add a marker to a clip in the media pool.")},
        {QStringLiteral("add_title"), QStringLiteral("Add a title clip with text to the timeline.")},
        {QStringLiteral("add_effect"), QStringLiteral("Add an effect to a clip on the timeline.")},
        {QStringLiteral("add_subtitle"), QStringLiteral("Add a subtitle to the timeline.")},
        {QStringLiteral("add_track"), QStringLiteral("Add a video or audio track to the timeline.")},
        {QStringLiteral("add_marker"), QStringLiteral("Add a timeline marker with a comment.")},
        {QStringLiteral("add_transition"), QStringLiteral("Add a cross-dissolve transition between two clips.")},
        {QStringLiteral("add_effect_keyframe"), QStringLiteral("Add a keyframe to an effect on a clip in the timeline.")},
        {QStringLiteral("create_bin_folder"), QStringLiteral("Create a folder in the media pool.")},
        {QStringLiteral("rename_bin_clip"), QStringLiteral("Rename a clip in the media pool.")},
        {QStringLiteral("get_media_pool"), QStringLiteral("List all clips in the media pool (or a specific folder).")},
        {QStringLiteral("build_timeline"), QStringLiteral("Build a complete timeline: import media, sequence clips, add transitions, optionally add audio.")},
        {QStringLiteral("append_clips"),
         QStringLiteral("Append multiple clips sequentially on a track. bin_ids: List of media pool clip IDs to append in order.")},
        {QStringLiteral("insert_clip"), QStringLiteral("Insert a single clip at position. bin_id: Media pool clip ID (string).")},
    };
    QJsonArray tools;
    for (const auto &def : defs) {
        tools.append(compactTool(mcpTool(def.first, def.second)));
    }
    const QStringList ranked = rankTools(tools, QStringLiteral("lets take all of the photos in the bin and add them to the timeline"), 6);
    CHECK(ranked.contains(QLatin1String("append_clips")));
    CHECK(ranked.contains(QLatin1String("insert_clip")));
    // The specific words should beat the one generic verb: a tool that only matches "add" is ranked lower, or not at all.
    const int marker = int(ranked.indexOf(QLatin1String("add_marker")));
    CHECK((marker < 0 || ranked.indexOf(QLatin1String("append_clips")) < marker));
}

TEST_CASE("The starting model is one that can call tools", "[AiChat]")
{
    // Ollama lists alphabetically, so the first entry is often a model that cannot call tools.
    const QStringList installed = {QStringLiteral("codellama:7b-instruct-q4_0"), QStringLiteral("hermes3:8b"),
                                   QStringLiteral("mistral:7b-instruct-q4_0"),   QStringLiteral("qwen2.5-coder:14b"),
                                   QStringLiteral("qwen2.5:7b-instruct"),        QStringLiteral("tinyllama:latest")};
    CHECK(preferredModel(installed) == QLatin1String("qwen2.5-coder:14b"));
    CHECK(preferredModel({QStringLiteral("codellama:latest"), QStringLiteral("hermes3:8b")}) == QLatin1String("hermes3:8b"));
    CHECK(preferredModel({QStringLiteral("qwen2.5:7b-instruct"), QStringLiteral("qwen3:8b")}) == QLatin1String("qwen2.5:7b-instruct"));
    // Nothing known: fall back to the first, and to nothing for an empty list.
    CHECK(preferredModel({QStringLiteral("gemma:2b"), QStringLiteral("phi3")}) == QLatin1String("gemma:2b"));
    CHECK(preferredModel(QStringList()).isEmpty());
}

TEST_CASE("Failures the tools report as text are recognised", "[AiChat]")
{
    CHECK(reportsFailure(QStringLiteral("ERROR: Kdenlive isn't running")));
    CHECK(reportsFailure(QStringLiteral("  ERROR: leading space")));
    CHECK_FALSE(reportsFailure(QStringLiteral("Marker added at frame 10")));
    CHECK_FALSE(reportsFailure(QStringLiteral("No ERROR found in the clip")));
    CHECK_FALSE(reportsFailure(QString()));
}

TEST_CASE("Tools that leave the project alone are told apart from those that change it", "[AiChat]")
{
    // Looking around, moving the playhead, selecting and previewing never need asking about.
    for (const QString &name : {QStringLiteral("get_timeline_summary"), QStringLiteral("find_clip"), QStringLiteral("list_clips"), QStringLiteral("seek_to"),
                                QStringLiteral("add_to_selection"), QStringLiteral("render_frame"), QStringLiteral("detect_scenes")}) {
        INFO(name.toStdString());
        CHECK(isReadOnlyTool(name));
    }
    // Everything that edits does, including taking edits back, and unknown names count as changes.
    for (const QString &name :
         {QStringLiteral("append_clips"), QStringLiteral("delete_clip"), QStringLiteral("move_clip"), QStringLiteral("undo"), QStringLiteral("redo"),
          QStringLiteral("add_marker"), QStringLiteral("render_video"), QStringLiteral("import_media"), QStringLiteral("some_new_tool")}) {
        INFO(name.toStdString());
        CHECK_FALSE(isReadOnlyTool(name));
    }
}

TEST_CASE("Risky tools and placeholders are recognised", "[AiChat]")
{
    CHECK(isRiskyTool(QStringLiteral("new_project")));
    CHECK(isRiskyTool(QStringLiteral("render_video")));
    CHECK_FALSE(isRiskyTool(QStringLiteral("delete_clip")));
    CHECK_FALSE(isRiskyTool(QStringLiteral("get_project_info")));

    QJsonObject args;
    args.insert(QStringLiteral("track"), 1);
    args.insert(QStringLiteral("name"), QStringLiteral("intro.mp4"));
    CHECK(placeholderArgument(args).isEmpty());
    args.insert(QStringLiteral("clip_id"), QStringLiteral("<clip-id>"));
    CHECK(placeholderArgument(args) == QLatin1String("clip_id"));
}

TEST_CASE("Long tool output is cut with a note", "[AiChat]")
{
    CHECK(truncateForModel(QStringLiteral("short"), 100) == QLatin1String("short"));
    const QString cut = truncateForModel(QString(50, QLatin1Char('x')), 10);
    CHECK(cut.startsWith(QString(10, QLatin1Char('x'))));
    CHECK(cut.contains(QLatin1String("40 more characters")));
}

TEST_CASE("History is trimmed oldest first", "[AiChat]")
{
    auto message = [](Message::Role role, const QString &text) {
        Message m;
        m.role = role;
        m.content = text;
        return m;
    };
    const QString big(1000, QLatin1Char('x'));
    QList<Message> history;
    history << message(Message::Role::User, QStringLiteral("first"));
    history << message(Message::Role::Assistant, QStringLiteral("looking"));
    history << message(Message::Role::Tool, big);
    history << message(Message::Role::Assistant, QStringLiteral("done"));
    history << message(Message::Role::User, QStringLiteral("second"));
    history << message(Message::Role::Assistant, QStringLiteral("looking"));
    history << message(Message::Role::Tool, big);

    SECTION("Within budget nothing changes")
    {
        QList<Message> copy = history;
        trimHistory(copy, 100000);
        CHECK(copy.size() == history.size());
        CHECK(copy.at(2).content == big);
    }
    SECTION("Old tool output goes first and the latest turn's output is kept")
    {
        trimHistory(history, 1500);
        REQUIRE(history.size() == 7);
        CHECK(history.at(2).content != big);
        CHECK(history.at(6).content == big);
    }
    SECTION("Then whole old turns go, but never the latest")
    {
        trimHistory(history, 500);
        REQUIRE(history.size() == 3);
        CHECK(history.at(0).content == QLatin1String("second"));
        CHECK(history.at(2).content == big);
    }
}

TEST_CASE("MCP replies are read from JSON and server-sent events", "[AiChat]")
{
    QJsonObject response;
    QString error;

    SECTION("A server-sent event, with CRLF line ends")
    {
        const QByteArray body = sseBody(QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"tools\":[]}}")).toUtf8();
        REQUIRE(McpClient::parseResponse(body, QStringLiteral("text/event-stream"), 7, &response, &error));
        CHECK(response.contains(QLatin1String("result")));
    }
    SECTION("Plain JSON")
    {
        const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32601,\"message\":\"nope\"}}";
        REQUIRE(McpClient::parseResponse(body, QStringLiteral("application/json"), 3, &response, &error));
        CHECK(response.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString() == QLatin1String("nope"));
    }
    SECTION("A data payload split over several data lines")
    {
        const QByteArray body = "event: message\ndata: {\"jsonrpc\":\"2.0\",\ndata: \"id\":1,\"result\":{}}\n\n";
        REQUIRE(McpClient::parseResponse(body, QStringLiteral("text/event-stream; charset=utf-8"), 1, &response, &error));
    }
    SECTION("A reply for another request, or none at all, is not accepted")
    {
        const QByteArray body = sseBody(QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}")).toUtf8();
        CHECK_FALSE(McpClient::parseResponse(body, QStringLiteral("text/event-stream"), 9, &response, &error));
        CHECK_FALSE(error.isEmpty());
        error.clear();
        CHECK_FALSE(McpClient::parseResponse(QByteArray(), QStringLiteral("application/json"), 1, &response, &error));
        CHECK_FALSE(error.isEmpty());
    }
}

TEST_CASE("Tool results are flattened to text", "[AiChat]")
{
    bool isError = true;
    SECTION("Text parts are joined")
    {
        const QJsonObject result = QJsonDocument::fromJson("{\"content\":[{\"type\":\"text\",\"text\":\"a\"},{\"type\":\"text\",\"text\":\"b\"}]}").object();
        CHECK(McpClient::resultText(result, &isError) == QLatin1String("a\nb"));
        CHECK_FALSE(isError);
    }
    SECTION("The error flag and non-text content are reported")
    {
        const QJsonObject result = QJsonDocument::fromJson("{\"isError\":true,\"content\":[{\"type\":\"image\",\"data\":\"AAAA\"}]}").object();
        CHECK(McpClient::resultText(result, &isError) == QLatin1String("[image content not shown]"));
        CHECK(isError);
    }
    SECTION("Structured content stands in when there is no text")
    {
        const QJsonObject result = QJsonDocument::fromJson("{\"content\":[],\"structuredContent\":{\"result\":\"ok\"}}").object();
        CHECK(McpClient::resultText(result, &isError) == QLatin1String("{\"result\":\"ok\"}"));
    }
    SECTION("Nothing at all")
    {
        CHECK(McpClient::resultText(QJsonObject(), &isError) == QLatin1String("(no output)"));
    }
}

TEST_CASE("An Ollama stream is assembled across chunk boundaries", "[AiChat]")
{
    ChatStreamParser parser(Provider::Ollama);
    const QByteArray stream = "{\"message\":{\"role\":\"assistant\",\"content\":\"Hel\"},\"done\":false}\n"
                              "{\"message\":{\"role\":\"assistant\",\"content\":\"lo\"},\"done\":false}\n"
                              "{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"function\":{\"name\":\"find_clip\",\"arguments\":{"
                              "\"name\":\"x\"}}}]},\"done\":false}\n"
                              "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true,\"done_reason\":\"stop\"}\n";
    QString streamed;
    // Feed it in awkward pieces so lines are cut in the middle.
    for (int i = 0; i < stream.size(); i += 17) {
        streamed += parser.feed(stream.mid(i, 17));
    }
    streamed += parser.finish();
    CHECK(streamed == QLatin1String("Hello"));
    CHECK(parser.content() == QLatin1String("Hello"));
    REQUIRE(parser.toolCalls().size() == 1);
    CHECK(parser.toolCalls().at(0).name == QLatin1String("find_clip"));
    CHECK(parser.toolCalls().at(0).arguments.value(QLatin1String("name")).toString() == QLatin1String("x"));
    CHECK(parser.error().isEmpty());

    SECTION("An error line is reported")
    {
        ChatStreamParser failing(Provider::Ollama);
        failing.feed("{\"error\":\"model requires more memory\"}\n");
        CHECK(failing.error() == QLatin1String("model requires more memory"));
    }
    SECTION("A last line without a newline is still read by finish()")
    {
        ChatStreamParser tail(Provider::Ollama);
        CHECK(tail.feed("{\"message\":{\"content\":\"end\"},\"done\":true}").isEmpty());
        CHECK(tail.finish() == QLatin1String("end"));
    }
}

TEST_CASE("An OpenAI-style stream stitches tool-call fragments together", "[AiChat]")
{
    ChatStreamParser parser(Provider::OpenAiCompatible);
    const QByteArray stream =
        ": keep-alive\n\n"
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Sure\"}}]}\n\n"
        "data: "
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_9\",\"function\":{\"name\":\"split_clip\",\"arguments\":\"{\\\"clip\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"_id\\\": \\\"4\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call_10\",\"function\":{\"name\":\"undo\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    QString streamed;
    for (int i = 0; i < stream.size(); i += 23) {
        streamed += parser.feed(stream.mid(i, 23));
    }
    streamed += parser.finish();
    CHECK(streamed == QLatin1String("Sure"));
    const QList<ToolCall> calls = parser.toolCalls();
    REQUIRE(calls.size() == 2);
    CHECK(calls.at(0).id == QLatin1String("call_9"));
    CHECK(calls.at(0).name == QLatin1String("split_clip"));
    CHECK(calls.at(0).arguments.value(QLatin1String("clip_id")).toString() == QLatin1String("4"));
    CHECK(calls.at(1).name == QLatin1String("undo"));
    CHECK(parser.error().isEmpty());

    SECTION("An error object in the stream is reported")
    {
        ChatStreamParser failing(Provider::OpenAiCompatible);
        failing.feed("data: {\"error\":{\"message\":\"rate limited\"}}\n\n");
        CHECK(failing.error() == QLatin1String("rate limited"));
    }
}

TEST_CASE("Requests are shaped for each provider", "[AiChat]")
{
    LlmRequest request;
    request.model = QStringLiteral("m");
    request.contextSize = 4096;
    Message system;
    system.role = Message::Role::System;
    system.content = QStringLiteral("rules");
    Message assistant;
    assistant.role = Message::Role::Assistant;
    ToolCall call;
    call.id = QStringLiteral("call_1");
    call.name = QStringLiteral("find_clip");
    call.arguments.insert(QStringLiteral("name"), QStringLiteral("x"));
    assistant.toolCalls << call;
    Message tool;
    tool.role = Message::Role::Tool;
    tool.toolCallId = QStringLiteral("call_1");
    tool.toolName = QStringLiteral("find_clip");
    tool.content = QStringLiteral("42");
    request.messages << system << assistant << tool;

    SECTION("Ollama takes arguments as an object, names the tool, and gets num_ctx")
    {
        request.tools.append(QJsonObject());
        const QJsonObject body = LlmClient::requestBody(Provider::Ollama, request);
        CHECK(body.value(QLatin1String("stream")).toBool());
        CHECK(body.value(QLatin1String("options")).toObject().value(QLatin1String("num_ctx")).toInt() == 4096);
        CHECK(body.contains(QLatin1String("tools")));
        const QJsonArray messages = body.value(QLatin1String("messages")).toArray();
        const QJsonObject sentCall =
            messages.at(1).toObject().value(QLatin1String("tool_calls")).toArray().at(0).toObject().value(QLatin1String("function")).toObject();
        CHECK(sentCall.value(QLatin1String("arguments")).isObject());
        CHECK(messages.at(2).toObject().value(QLatin1String("tool_name")).toString() == QLatin1String("find_clip"));
    }
    SECTION("OpenAI takes arguments as a string, links results by id, and has no num_ctx")
    {
        const QJsonObject body = LlmClient::requestBody(Provider::OpenAiCompatible, request);
        CHECK_FALSE(body.contains(QLatin1String("options")));
        CHECK_FALSE(body.contains(QLatin1String("tools")));
        const QJsonArray messages = body.value(QLatin1String("messages")).toArray();
        const QJsonObject sentCall = messages.at(1).toObject().value(QLatin1String("tool_calls")).toArray().at(0).toObject();
        CHECK(sentCall.value(QLatin1String("id")).toString() == QLatin1String("call_1"));
        CHECK(sentCall.value(QLatin1String("type")).toString() == QLatin1String("function"));
        CHECK(sentCall.value(QLatin1String("function")).toObject().value(QLatin1String("arguments")).isString());
        CHECK(messages.at(2).toObject().value(QLatin1String("tool_call_id")).toString() == QLatin1String("call_1"));
    }
}
