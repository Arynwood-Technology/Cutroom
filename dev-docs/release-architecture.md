<!--
    SPDX-FileCopyrightText: 2026 Arynwood Technology
    SPDX-License-Identifier: GPL-3.0-only
-->

# Arynwood Cutroom: release architecture

How Arynwood Cutroom gets from a Linux-only alpha built from source to a branded release
on Linux, macOS and Windows. Decisions are numbered so other documents can point at them.
**Status: design, written 2026-09-20.** What is built is marked *done*; everything else is a
proposal that a later phase may revise. Commercial terms (pricing, budgets) are out of scope
here.

## Where things stand

Cutroom is a Kdenlive fork with the D-Bus scripting API from
[D-Ogi's fork](https://github.com/D-Ogi/kdenlive), the in-editor Cutroom Assistant
(`src/aichat/`), 183 MCP tools (`mcp-kdenlive`), and Arynwood branding. It builds as a
flatpak and is tested on Linux only.

Constraints found in the code that shape every decision below:

- **The tool plane is Linux-only.** D-Bus is compiled in only on Linux/BSD
  (`CMakeLists.txt`, `USE_DBUS`, default OFF elsewhere); the service name is claimed in
  `src/main.cpp` under `#ifndef NODBUS`; the Python client (`kdenlive-api`) shells out to
  `dbus-send`, `gdbus` or `qdbus`. Every call passes through one method, `_call`.
- **The tool server is a separate process** that the panel does not start; it only stores
  its URL (`aiMcpUrl`).
- **The scriptable surface is generic.** About 190 `Q_SCRIPTABLE` methods in
  `src/mainwindow.h`, using only `bool`, `int`, `double`, `QString`, `QStringList` and
  `QList<int>` as parameters, and `bool`, `int`, `double`, `QString`, `QStringList`,
  `QVariantList`, `QVariantMap` or `void` as results. All of it maps to JSON.
- **Upstream ships all three platforms** with KDE Craft (`.craft.ini`, `.gitlab-ci.yml`),
  on KDE's own CI, which this project cannot use.

## Decisions

| # | Decision | Status |
|---|---|---|
| D1 | **Rebrand at the seams, not in the guts.** Only what users see and what the operating system registers is Cutroom's own: the application ID `com.arynwood.Cutroom`, desktop entry, AppStream metainfo, icons, D-Bus service name, About, installers. Internal names stay (`kdenlive` component, the `org.kde.kdenlive*` QML module names, C++ namespaces, the D-Bus *interface* names, the `.kdenlive` project format). That keeps merges from upstream and D-Ogi cheap and keeps project files interchangeable with Kdenlive. | Linux: **done** (see the D-Bus naming rule below) |
| D2 | **Side by side with stock Kdenlive.** A flatpak isolates its settings by application ID. Other packagings would share `kdenliverc`, `kdenlive-layoutsrc` and the data directory with a real Kdenlive, so they need a settings and data namespace (`KConfig::setMainConfigName` and an application data path) before they ship. | Proposed |
| D3 | **An in-app Automation Server replaces D-Bus as the tool transport.** `QLocalServer` (a Unix socket on Linux and macOS, a named pipe on Windows), newline-delimited JSON-RPC 2.0, with one generic `call(method, args)` dispatched to the `Q_SCRIPTABLE` methods through `QMetaObject`, plus a `list_methods` introspection call. Local user only, no TCP port. D-Bus stays as an optional Linux transport. `kdenlive-api` gets a `Transport` interface behind `_call`. | Proposed. The dispatcher exists and is tested (`src/automation/`, `tests/automationdispatchertest.cpp`); the server and the client transport do not. |
| D4 | **The tool server ships inside the app as a managed sidecar over stdio.** A `ToolServerManager` starts `cutroom-mcp` when the assistant opens, restarts it if it crashes and stops it on exit; `AiMcpClient` gains a stdio transport and the MCP `initialize` handshake. The same entry point is offered as a `cutroom-mcp` command for other agents. Runtime: the KDE runtime's Python plus vendored wheels in the flatpak; python-build-standalone plus pinned wheels on Windows and macOS. The current `aiMcpUrl` becomes an optional "external server" override. | Proposed |
| D5 | **Models: none bundled, no hosted service.** Ollama (available on all three systems) by default, OpenAI-compatible endpoints optional. A first-run check uses Ollama's `/api/show` capabilities to mark models that can call tools. Compatibility claims come only from the benchmark harness (D9). | Proposed |
| D6 | **Channels.** Linux: a flatpak `com.arynwood.Cutroom` (own repository or bundle first; Flathub later, which verifies domain-based IDs with a file under `.well-known/` on the domain) and an AppImage through Craft. Windows: a Craft MSVC x64 installer and zip. macOS: a Craft DMG, arm64 first. No snap or deb. | Proposed |
| D7 | **Build system: KDE Craft on GitHub Actions** for Windows, macOS and AppImage; flatpak-builder for Linux. Craft downloads prebuilt Qt, KDE Frameworks and MLT from KDE's cache, so only this repository compiles. The GitLab templates in `.gitlab-ci.yml` stay unused. | Proposed |
| D8 | **Versioning and updates.** Cutroom has its own version (`0.1.0-alpha` to `0.9.x` beta to `1.0`), shown beside the Kdenlive version it is based on. Releases carry the artifacts, SHA-256 sums, an SPDX bill of materials and the corresponding-source tarball. No automatic updates in the first releases; a later opt-in, default-off update check. **No telemetry.** | Proposed |
| D9 | **Quality gates.** The 34-test suite plus tests for each new component; an assistant benchmark harness (about ten canonical tasks run against a dry-run tool proxy, a scripted fake model for regression in CI, real models for a published compatibility table); a cross-platform smoke test (launch, open a sample project, render five seconds, launch the assistant with a fake model, Allow and Deny); a side-by-side test with stock Kdenlive. | Proposed |
| D10 | **Licence and brand.** The application is GPL-3.0-only; `mcp-kdenlive` and `kdenlive-api` are MIT; the logo is all rights reserved (`TRADEMARKS.md`). Third-party notices and the source offer appear in About and on the website. "Kdenlive" is used only descriptively ("based on"). Legal review of the logo licence, the name and the GPL compliance package is required before 1.0. | Draft |
| D11 | **Signing policy.** Windows and macOS builds that are not signed and notarised go only to invited testers, with the SmartScreen and Gatekeeper steps documented. Signed and notarised builds are required before any public Windows or macOS download. | Proposed |

### D-Bus naming inside a flatpak (found while doing D1)

A sandboxed app may only own D-Bus names at or below its application ID. KDE's `KDBusService`
names its service from the organisation domain reversed plus the component name, so keeping
`kde.org` would ask for `org.kde.kdenlive.*` and be refused, and the app quits at startup.
`src/main.cpp` therefore sets the organisation domain to `Cutroom.arynwood.com` (not a real
host), which yields `com.arynwood.Cutroom.kdenlive*`; the scripting name `com.arynwood.Cutroom`
is claimed explicitly. `kdenlive-api` matches any name that starts with `com.arynwood.Cutroom`
followed by `.` or `-`. Only the installed `flatpak run` enforces this, which is why the
release test runs it (`dev-docs/release-testing.md`).

The same domain also feeds QtDBus, which names an exported object's *interface* from it unless
the class pins one. Left alone, the scripting interface became
`com.arynwood.cutroom.kdenlive.MainWindow`, silently breaking every client. `MainWindow` now
carries `Q_CLASSINFO("D-Bus Interface", "org.kde.kdenlive.MainWindow")` (`src/mainwindow.h`),
and `tests/automationdispatchertest.cpp` fails if that pin is lost. KDE's own window interfaces
(`KMainWindow`, `KXmlGuiWindow`, `KActionCollection`) follow the domain and are now
`com.arynwood.cutroom.kdenlive.*`; nothing in Cutroom's tooling uses them.

## Automation Server sketch (D3)

- **Endpoint.** Linux: `$XDG_RUNTIME_DIR/cutroom/automation-<pid>.sock` (inside a flatpak,
  `$XDG_RUNTIME_DIR/app/com.arynwood.Cutroom/`, which the host can also reach, to be
  verified). macOS: a socket under the user's cache directory. Windows: the named pipe
  `\\.\pipe\cutroom-<user>-<pid>`. A small instances file lists running instances and their
  versions so clients can choose among several.
- **Access.** Owner-only permissions on the socket or pipe; the server never listens on a TCP
  port. Any process of the same user could already reach the D-Bus interface, so the trust
  boundary does not widen.
- **Protocol.** JSON-RPC 2.0, one message per line. `cutroom.listMethods` returns the
  scriptable methods with parameter and result types from the meta-object;
  `cutroom.call {method, args}` invokes one. Type conversion is `QJsonValue` to the
  parameter's declared type and back; a mismatch is an error and is never coerced silently.
- **Not in the server.** Approval policy stays in the assistant panel, as it does today.
  The server is transport only.

## Phases and exit gates

| Phase | Deliverable | Exit gate |
|---|---|---|
| **P0 Foundation** (done 2026-09-20) | This document; Linux application ID; repository files; a dispatcher spike | The new-ID flatpak builds and launches, and the assistant and the scripting client work under it |
| **P1 Linux alpha `0.1.0-alpha`** | `ToolServerManager` and the bundled `cutroom-mcp`; model check; alpha documentation; a release with the flatpak bundle, checksums and source tarball | A fresh Linux machine reaches a working assistant in under ten minutes with no manual service setup |
| **P2 Automation Server** | Dispatcher, `QLocalServer`, the `Transport` interface in `kdenlive-api`, settings and data namespace | `kdenlive-api` tests pass over the local socket; the assistant works with no D-Bus present |
| **P3 Windows beta** | Craft MSVC build with `USE_DBUS=OFF`, the sidecar, an installer and zip, for testers | Full smoke test on a real Windows machine; expression engine and libplacebo build or are switched off in CMake |
| **P4 macOS beta** | An arm64 DMG, then x86-64, for testers | Full smoke test on a real Mac, including OpenGL and audio checks |
| **P5 Public beta and 1.0** | Signing in CI, legal review done, Flathub submission, package-manager entries, Cutroom's own handbook for F1, website downloads | Compatibility table published; no known data-loss bugs; signed installs verified on clean machines; side-by-side verified |

Windows and macOS are the schedule risk: this fork has not yet been built with Craft on
either. Expect the first green build there to be the largest unknown.

## Risks

- **Craft and the fork's additions** (expression engine, libplacebo) on Windows and macOS.
  Mitigation: switch them off in CMake rather than block a release.
- **Flathub builds from source.** Vendored Python wheels (for example `pydantic-core`, which
  needs Rust) complicate that. Mitigation: own repository or bundle first.
- **Model reliability.** On the benchmark task in `src/aichat/README.md` the best local model
  was right one time in three. The panel asks before every change by default, and the
  compatibility table keeps claims to what was measured.
- **Upstream drift.** This is a fork of a fork of Kdenlive; D1 keeps internal names to keep
  merges cheap.
