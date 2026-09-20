<!--
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
-->

# Testing an Arynwood Cutroom release

What was run to gate the first release, and how to run it again. All commands
assume the flatpak toolchain from `SETUP.md` and a checkout in
`~/GitHub/kdenlive-arynwood`.

## 1. Static gates

- No secrets or personal paths in new or changed files:
  `grep -rnE "/home/[a-z]+|sk-[A-Za-z0-9]{16,}|BEGIN (RSA|OPENSSH|PRIVATE)"` over the diff.
- Every new file has an SPDX header (`reuse lint` when the `reuse` tool is installed).
- `appstreamcli validate --no-net data/com.arynwood.Cutroom.metainfo.xml` and
  `desktop-file-validate data/com.arynwood.Cutroom.desktop`.
- Branding: the window title, About dialog, welcome screens and Help menu must say
  Arynwood Cutroom and credit Kdenlive. See "Known release blockers" below.

## 2. The project's own test suite (34 tests, including `aichattest`)

The flatpak build turns tests off, and its cleanup step strips the CMake configs and
headers from the finished app directory, so the suite needs a build directory that
stops *before* cleanup:

```bash
cd ~/GitHub/kdenlive-arynwood
flatpak-builder --user --force-clean --disable-updates --stop-at=kdenlive \
    ~/.cache/kdenlive-deps-build .flatpak-manifest.json

flatpak-builder --run ~/.cache/kdenlive-deps-build .flatpak-manifest.json bash -c '
  mkdir -p ~/.cache/kdenlive-tests-build && cd ~/.cache/kdenlive-tests-build
  cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=/app \
        -DCMAKE_INSTALL_PREFIX=/app -DRELEASE_BUILD=OFF -DBUILD_TESTING=ON ~/GitHub/kdenlive-arynwood
  ninja
  # The tests look for the effect and transition definitions in an installed location.
  DESTDIR=~/.cache/kdenlive-tests-install ninja install
  export QT_QPA_PLATFORM=offscreen XDG_DATA_DIRS=$HOME/.cache/kdenlive-tests-install/app/share:/app/share:/usr/share
  ctest --timeout 600 --output-on-failure -j6'
```

Without the `XDG_DATA_DIRS` line five tests fail (`effectstest`, `filetest`,
`keyframetest`, `mixtest`, `modeltest`), all with `unordered_map::at`, because no
effects are registered. That is the test environment, not the code.

## 3. End to end, in an isolated instance

Never test against your working Cutroom or your real `mcp-kdenlive`: the assistant
changes the open project. The isolated setup that was used:

- a private D-Bus (`dbus-daemon --session --address=unix:path=...`) and an invisible
  display (`Xvfb :99 +extension GLX`); the app runs with `DISPLAY=:99` and
  `DBUS_SESSION_BUS_ADDRESS` set to the private bus;
- its own config and data (`XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`).
  **The flatpak build reads `kdenlive-flatpakrc`, not `kdenliverc`.** Seed
  `[aiassistant]` with `aiOllamaUrl`, `aiMcpUrl` and `aiOpenAiUrl` in that file;
- its own `mcp-kdenlive` on another port (`MCP_TRANSPORT=http MCP_PORT=18420`)
  started with the private bus address;
- a scripted stand-in for the model that speaks both Ollama's and the OpenAI
  dialect, so runs are deterministic (a real 8B model got the photo task exactly
  right about one time in three), and whose OpenAI side rejects a history with an
  unanswered `tool_calls`, as OpenAI does.

**Prove the isolation before sending anything.** A wrong config file name sent the
first attempt to the real Ollama and the real tool server. Check that the stand-in
model logged the panel's `GET /api/tags` and that the isolated MCP server logged a
request from the app.

Checks, with what each one needs to show:

| Check | Pass condition |
|---|---|
| First run | Welcome screen says Arynwood Cutroom, shows the tree, credits Kdenlive; main window in the Purple scheme; window title ends in "Arynwood Cutroom" |
| Panel | Nothing opens at startup. The "Launch Assistant" button (Arynwood tree, top right of the menu bar, left of the workspace tabs) and View → Cutroom Assistant both open it as a tab in the Clip Monitor group, not a floating window, with the cursor in the message box. Clicking the button again with the panel hidden behind another tab brings it to the front. |
| Identity | Run the **installed** flatpak (`flatpak run com.arynwood.Cutroom`, with the private bus and display below), not `flatpak-builder --run`: the app stays up; `ListNames` on the private bus shows `com.arynwood.Cutroom` and names below it (`com.arynwood.Cutroom.kdenlive...`) and **no** `org.kde.kdenlive*` (use `dbus-send --bus=unix:path=<bus>`, not `--address`); the window's `_KDE_NET_WM_DESKTOP_FILE` is `com.arynwood.Cutroom`; `gdbus introspect --session --dest com.arynwood.Cutroom --object-path /MainWindow` shows the interface **`org.kde.kdenlive.MainWindow`**; and a real `kdenlive-api` call (`KdenliveDBus()._call("scriptGetProjectFps")`, run with `DBUS_SESSION_BUS_ADDRESS` set to the private bus) returns a value |
| Bug reports | Help → Report Bug… opens `https://arynwood.com/#contact` and no KDE dialog; About → Authors and About → About both say to report to Arynwood, not KDE. There is no Donate item in the Help menu of this build; the only donation links are on the splash screens and go to `kdenlive.org/fund`, deliberately, until Cutroom has its own donation route |
| Default model | The first tool-capable model is picked, not the first alphabetically |
| Allow | Read tools run unasked; `append_clips` waits for approval; nothing changes before Allow; afterwards the timeline has the clips |
| Deny | After Deny the clip count is unchanged and the model is told |
| Stop | Stop with an approval pending, then a follow-up message, gets an answer (on the OpenAI dialect: no rejected history) |
| Failure | A dead model server or tool server gives a plain message naming the address |
| Privacy | A non-local address shows the orange warning and, on send, the one-time notice. Use a name that cannot resolve (`ollama.invalid`) so no test text leaves the machine |
| Persistence | A colour scheme chosen by the user survives a restart and is not reset to purple; the chosen model survives |
| Scrolling | After several exchanges and after a tool call with an approval, the newest lines are visible without scrolling |

Traps met while doing this:

- `import -window root` returns black for the GL-rendered window; capture the window by id.
- `flatpak-builder --run <dir>` can silently run a *previous* build if a stale
  `rofiles-fuse` mount for an older build directory is still there. Unmount your own
  stale mounts and compare `md5sum /proc/<pid>/exe` with the build's `files/bin/kdenlive`
  before trusting a result.
- A private D-Bus is not private from the desktop: the private bus starts its own
  `xdg-desktop-portal`, which hands "open this URL" requests (Help → Report Bug…,
  any link) to the *real* default browser on your real display. Watch such clicks
  with `dbus-monitor --address unix:path=<private bus>` (look for
  `org.freedesktop.portal.OpenURI`) and expect a tab to open on your desktop.
- **`flatpak-builder --run` does not enforce the sandbox's D-Bus name rules; the installed
  `flatpak run` does.** A build that passes the isolated tests above can still quit at
  startup once installed. The application ID is the only D-Bus name space the app may own,
  and KDE's `KDBusService` builds its name from the *organisation domain reversed* plus the
  component name, so the domain set in `src/main.cpp` must reverse into the app ID
  (`Cutroom.arynwood.com` gives `com.arynwood.Cutroom.kdenlive`). The symptom is
  `kf.dbusaddons: Failed to register name ...` in the log and no window. To test the
  installed app in isolation, start the private bus and `Xvfb` as above and run
  `DISPLAY=:99 DBUS_SESSION_BUS_ADDRESS=unix:path=<bus> flatpak run com.arynwood.Cutroom`, after
  seeding `~/.var/app/com.arynwood.Cutroom/config/kdenlive-flatpakrc` with dead-port
  `[aiassistant]` addresses; delete that folder afterwards so a real first run starts clean.
- **The organisation domain feeds more than the service name.** QtDBus also derives the D-Bus
  *interface* name of every exported object from it, so changing the domain renamed the scripting
  interface until `MainWindow` pinned it with `Q_CLASSINFO`. After any change to the domain or
  the app ID, introspect `/MainWindow` on the running installed app and make a real client call;
  reading the log is not enough.
- Changing the application ID invalidates flatpak-builder's cache: the next build compiles all
  28 dependencies again (80 minutes here), and later builds are cached again.
- Killing the app hard makes the next start show "Auto-saved file exists" and blocks
  scripting calls until it is answered.
- `pkill -f` and `pgrep -f` match their own shell's command line; match on process ids,
  ports or the private display instead.

## Known release blockers (branding and identity)

Found by the tests above and not yet changed, because each needs a decision:

- **Report Bug** now goes to `https://arynwood.com/#contact`, set once as the bug
  address in `main.cpp` (a stopgap until there is a public issue tracker: change
  that one string). **Donations** stay with the Kdenlive project on purpose, until
  there is a stable release and a donation route of its own.
- **The Handbook** (F1) opens Kdenlive's documentation.
- **Identity on Linux is done**: the application ID is `com.arynwood.Cutroom`, with its
  own desktop entry, AppStream metainfo, icons and D-Bus service name, so it can be
  installed beside Kdenlive (`dev-docs/release-architecture.md`, decision D1). Still to
  do: a 1024 px master of the logo for macOS/Windows icons, a config and data-directory
  namespace so non-flatpak builds do not share `kdenliverc` with a real Kdenlive, and the
  Windows/macOS bundle identity.
- The Arynwood logo is recorded as "all rights reserved, not GPL"
  (`LICENSES/LicenseRef-Arynwood-Logo.txt`); that wording is a draft for counsel.
- The publishing entity (Arynwood Technology or Lorelei Noble) is still open.
