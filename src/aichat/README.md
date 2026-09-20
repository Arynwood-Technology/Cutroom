<!--
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
-->

# Cutroom Assistant (`src/aichat`)

The Cutroom Assistant is a dock panel that lets you edit the open project by
chatting with a model. It is part of Arynwood Cutroom, a build of Kdenlive, and
of the Arynwood open toolkit for local AI tools.

It does not reimplement the editing tools. It is a **client of the same
`mcp-kdenlive` tool server** that Claude Code and Arynwood MCP use, so the
assistant, an MCP client and a script all reach the editor through one tool
surface (`mcp-kdenlive` → D-Bus → `MainWindow`).

```
 Cutroom Assistant panel ──HTTP──► Ollama /api/chat  or  OpenAI-style /chat/completions
        │
        └────────HTTP (MCP)────► mcp-kdenlive :8420 ──D-Bus──► this Cutroom process
```

## Files

| File | Role |
|---|---|
| `aiassistantwidget.*` | The panel: transcript, model picker, status, approval bar, settings dialog. View only. |
| `aiagent.*` | The conversation loop. Sends history to the model, runs the tools it asks for, feeds results back until it answers in text. |
| `aillmclient.*` | Streams a chat completion from Ollama or an OpenAI-compatible server; `ChatStreamParser` reads both stream formats. |
| `aimcpclient.*` | Minimal MCP streamable-HTTP client: `tools/list`, `tools/call`. |
| `aitoolutils.*` | Pure functions: tool ranking, history trimming, recovery of tool calls written as text, risky-tool and placeholder checks. |
| `aitypes.h` | `Message` and `ToolCall`, independent of provider. |

Everything except the widget is free of UI and of Kdenlive internals, and is
covered by `tests/aichattest.cpp`.

## Things that were learned the hard way

These are all handled in code and each has a test or was checked against the
live servers; they are listed so nobody has to rediscover them.

- **The tool list is too big for a small model.** `mcp-kdenlive` exposes about
  180 tools, roughly 20,000 tokens even after dropping `outputSchema` and
  `title` keys. Ollama's default context window is far smaller, and through
  its OpenAI-compatible `/v1` endpoint the window cannot be set at all: with
  every tool sent, a local model never saw its tools and just rambled. By
  default only the tools that match the request (plus a small core set and
  anything used recently) are sent, and the model gets a `search_tools`
  function to pull in others. "All tools" is a setting for large-context models.
  The ranking weights a word by how rare it is across the tool names and
  descriptions: unweighted, "add" (which starts a dozen tool names) crowded
  `append_clips` and `insert_clip` out of the offered set for "add the photos in
  the bin to the timeline", so the model was never shown them.
- **The MCP server answers in SSE framing** (`text/event-stream`, one
  `event: message` / `data: {...}` frame) even when run stateless.
- **`mcp-kdenlive` reports failures as text starting with `ERROR`**, not through
  the MCP `isError` flag, so a failed action would otherwise show as a success.
  See `reportsFailure()`.
- **Some local models write tool calls as JSON in the reply text** instead of
  the `tool_calls` field, sometimes several back to back or in a code fence, and
  sometimes with a call that has no name. `toolCallsFromContent()` recovers the
  first kind; nameless calls are dropped. Only a reply that *starts* with such
  JSON and has an `arguments` member counts, so prose is never executed.
- **A model that never stops calling tools** is cut off after a set number of
  rounds and made to answer from what it has.
- **Cancelling must leave the conversation valid.** OpenAI-style servers reject
  an assistant message with `tool_calls` that has no matching tool reply, so
  Stop answers every outstanding call ("Cancelled by the user").

## Safety and privacy

- **Approval.** By default every tool that changes the project waits for the
  user's Allow or Deny (`ApprovalMode::EveryChange`); tools that only read, move
  the playhead, select or make previews never ask (`isReadOnlyTool()`). The
  settings can relax this to `RiskyOnly` (`new_project`, `open_project`,
  `load_project`, `checkpoint_restore`, `render_video`, `export_subtitles`,
  `abort_render_job`; see `isRiskyTool()`) or `Never`. The default is strict
  because small local models flail: one checked model made about 70 edits in a
  single request.
- **Limits that hold whatever the approval mode.** At most 12 tool calls per
  model reply and 40 per request; after that the model is made to answer. A
  project-changing call with identical name and arguments is refused after two
  runs (reads may repeat, since the project may have changed in between).
- Many timeline edits are undoable through Kdenlive's undo stack, but not every
  scripted operation is, so the panel does not promise that a change can be taken
  back and tells the user to save a copy of anything important first.
- Placeholder arguments such as `<clip-id>` are refused before they reach the
  editor.
- The panel shows where the model runs (local, local network, remote). With a
  remote endpoint the conversation, including tool results with project, clip
  and file names, is sent to that service, and the panel says so. With Ollama
  on this machine nothing leaves it.
- The API key is stored in Kdenlive's configuration file, obfuscated but **not
  encrypted** (KConfig's `Password` type).
- The panel does no network I/O until it is first shown.

## Settings

Stored in the `aiassistant` group of `kdenlivesettings.kcfg` and edited from the
gear button in the panel: provider, server addresses, API key, tool-server
address, context size, tool rounds, tool selection and the approval switch. The
model is chosen from the picker in the panel.

## Testing

- Unit tests: `tests/aichattest.cpp` (Catch2, built with `-DBUILD_TESTING=ON`).
  They need no running services.
- Against the real thing: run the `mcp-kdenlive` service and Ollama, open the
  panel, and ask for a timeline summary. A model that cannot call tools makes
  Ollama answer HTTP 400 "does not support tools"; the panel shows that message.

## License

GPL-3.0-only, like the rest of this tree. This code is part of the Cutroom
application and is not the same thing as the MIT-licensed `mcp-kdenlive` and
`kdenlive-api` packages it talks to; see their `NOTICE.md` files for why the two
stay separate.

## Known limits

- Only Ollama and the OpenAI chat-completions dialect are supported. Anthropic's
  Messages API is a different shape and is not implemented.
- The MCP client does not do the session handshake and does not follow
  `nextCursor` when listing tools; `mcp-kdenlive` runs stateless and returns its
  whole list in one reply.
- A tool that returns an image gets `[image content not shown]`; the
  screenshot and preview tools return file paths, which work.
- The transcript is text and Markdown; there is no inline image preview yet.
- The two status dots are checked when the panel is opened and when the settings
  change, so a dot can stay green after a server goes down. The message shown when
  a message or a tool call fails is the authoritative one.
- The panel has a minimum width, and docking it can make the main window a few
  dozen pixels wider.
- The MCP server is a separate process. It is not bundled into the flatpak, so
  it has to be running (`SETUP.md`, step 3).
- **Reliability is the model's, and small models are not reliable.** On "add all
  the photos in the bin to the timeline" (three runs each, tools executed
  against a dry-run proxy so nothing changed), `qwen2.5-coder:14b` got the exact
  call right once, `hermes3:8b` and `mistral:latest` never did (no tool call, the
  wrong tool, or a runaway loop), and `gpt-oss:20b` did not answer in four
  minutes on a 12 GB card because part of it runs on the CPU. Three runs is a
  small sample, but the pattern is clear enough that the panel defaults to asking
  before every change.
- **The default model is chosen by name, not by capability.** `preferredModel()`
  prefers a short list of families. Ollama reports each model's capabilities
  (`/api/show`, `capabilities: [..., "tools"]`), which is the authoritative
  signal; using it to mark or hide models that cannot call tools is the obvious
  next step.
- **Tool descriptions steer small models.** In `mcp-kdenlive`, `append_clips` and
  `insert_clip` say to use `build_timeline` instead for "full assembly", but
  `build_timeline` imports a *folder on disk* and does not use clips already in
  the bin. For "add the photos in the bin" that redirect sends a model to the
  wrong tool. The panel's prompt works around it; the descriptions should be
  fixed at the source.
