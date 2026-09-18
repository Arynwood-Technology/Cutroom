# Arynwood Cutroom — Product Description

## One sentence

Cutroom is an AI-native video editor: a patched build of Kdenlive that gives
an AI agent real, precise control of the timeline — cut, arrange, transition,
render — through 182 individually callable tools, not a chat window bolted
onto the side of an editing suite.

## What it actually is

Three pieces, each independently real and independently useful:

1. **A patched Kdenlive** ([`D-Ogi/kdenlive`](https://github.com/D-Ogi/kdenlive))
   with a Qt `Q_SCRIPTABLE` D-Bus interface added to `MainWindow` — the one
   thing stock Kdenlive has no equivalent of at all.
2. **`kdenlive-api`** — a Python client library for that interface, with
   method and class names modeled on DaVinci Resolve's scripting API, MIT
   licensed, 82 passing unit tests against a mocked D-Bus backend.
3. **`mcp-kdenlive`** — an MCP (Model Context Protocol) server exposing 182
   tools built on that library, so any MCP client — Claude Code, Claude
   Desktop, or Arynwood MCP — can drive a real editing session from plain
   language.

## Who it's for

Editors and creators comfortable with Linux and local AI tooling, who want
an AI collaborator that actually operates their timeline instead of just
describing what to do next. Not a mass-market "type a prompt, get a video"
tool — the timeline, the tracks, the effects stack are all still there and
still yours; Cutroom adds a second way to reach them.

## What makes it different (verified, not asserted)

| | Kdenlive | Cloud AI editors | Cutroom |
|---|---|---|---|
| Manual timeline editing | yes | varies | yes |
| Edits from plain-language instructions | no | yes | yes |
| Footage stays on your machine | yes | no | yes |
| Scriptable / MCP tool-calling | no | no | yes |
| Built-in Creative Commons sourcing | 4 sources | no | 6 sources |
| Cost | free | subscription | free (alpha) |

Nothing else combines AI-driven editing with keeping footage local. Cloud
tools trade privacy for the AI; Kdenlive keeps privacy by not having the AI.
Cutroom is the only row with both.

Two concrete, shipped differentiators beyond the tool-calling layer itself:

- **Real Creative Commons media sourcing.** Beyond Kdenlive's existing
  Freesound/Pexels/Pixabay/archive.org providers, this fork adds **Jamendo**
  (CC-licensed music) and **Openverse**, and had to extend the provider
  engine itself (`ProviderModel::replacePlaceholders`) with offset-based
  pagination support to make Jamendo's API work — a real, small addition to
  the fork's own C++ engine, not configuration.
- **A visual identity of its own.** A dedicated `CutroomDark.colors` scheme
  built from the actual Arynwood mark's palette (teal `#2dd1c4`, wine
  `#843a6e`, steel blue `#5a96c7`, on a near-black ground), not a recolor of
  a stock theme.

## Current state, honestly

- 182 tools implemented and documented, spanning project/media/timeline
  management, transitions, effects and keyframes, subtitles, titles,
  compositions, groups, selection, sequencing, speed/time-remap, playback,
  navigation, zones, proxies, checkpoints/undo, preview/screenshot capture,
  and audio.
- Both Python libraries fully tested (82 + 9 passing) with no known failing
  tests as of this pass — two real bugs (a documented deadlock risk that
  turned out to already be fixed at the C++ source, verified by reading the
  implementation; and a live, still-broken sequential-clip-insert bug) were
  found and fixed during this work, not just described.
- **Not yet packaged.** No Flatpak build of the patched fork has been
  produced yet; the AppImage/`.deb` pipeline that exists for Arynwood MCP
  itself is a separate, already-solved problem for a different piece of
  software and doesn't cover this fork.
- **Not yet legally reviewed.** See each repo's `NOTICE.md` for the specific
  open questions (GPL obligations on the fork itself if it's ever
  distributed as a binary, trademark-adjacent naming, publishing entity) —
  flagged for counsel, not resolved by this document.

## The pitch, one paragraph

Every other "AI video editor" is either a cloud service that wants your
footage, or a chat window duct-taped to the side of software that was never
built to be driven that way. Cutroom is neither: it's the same Kdenlive
engine you'd already trust, with a real scripting interface built into it
from the start, wrapped in 182 tools an AI agent can call directly — so the
model isn't describing an edit, it's making one.
