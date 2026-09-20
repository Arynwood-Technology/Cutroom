<!--
    SPDX-FileCopyrightText: 2026 Arynwood Technology
    SPDX-License-Identifier: CC0-1.0
-->

# Security

## Reporting a vulnerability

Please report security problems **privately**, not in a public issue:
<https://arynwood.com/#contact>. Say which version you have (Help → About) and how to
reproduce the problem. Arynwood Cutroom is an early alpha maintained by a very small team, so
there is no guaranteed response time, but reports are read and taken seriously.

Only the latest alpha is supported.

## What to know about how Cutroom works

These are design facts, not bugs, and they are the places a security review should look
first.

- **The scripting interface can edit and render your projects.** On Linux, Cutroom exposes
  its scripting methods on the D-Bus session bus (`com.arynwood.Cutroom`). Any program
  running as your user in that session can call them. That is the same trust boundary as the
  rest of your desktop session, and it is how the assistant's tool server and other agents
  operate the editor. Do not run untrusted programs in the session you edit in.
- **The assistant acts on your open project.** By default every change waits for your Allow
  or Deny, and a request is limited to 12 tool calls per reply and 40 in all. Not every
  action can be undone, so save a copy of anything important first.
- **Where your text goes.** With Ollama on the same computer nothing leaves it. If you point
  the assistant at a remote service, your messages and the tool results, which include
  project, clip and file names, are sent there. The panel warns you. The media files
  themselves are not uploaded.
- **The API key is stored obfuscated, not encrypted,** in Cutroom's configuration file
  (KConfig's `Password` type). Treat that file as sensitive.
- **The tool server listens on the local machine only** (`127.0.0.1:8420` by default) and has
  no authentication of its own. Anything on the machine that can reach that port can ask it to
  operate the editor.
- **Cutroom's own additions send no telemetry.** Kdenlive's online features (the media
  providers, downloadable resources) contact their services when you use them.

## Third-party code

Cutroom is built on Kdenlive, MLT, FFmpeg, Qt and other libraries. A vulnerability in one of
them is best reported to that project as well; tell us too so a fixed version can be
packaged.
