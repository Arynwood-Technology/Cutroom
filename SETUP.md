# Setting this up on a new machine

This fork only matters because of the D-Bus scripting API it adds
(`org.kde.kdenlive.MainWindow` on the session bus, object path `/MainWindow`).
Stock Kdenlive from Flathub/apt does **not** have this — you have to build
this fork's flatpak yourself. There is no way to "just copy" a working
install between machines; the flatpak build is tied to the target machine's
runtimes/toolchain.

## 1. Build and install the flatpak

Prerequisites:

```bash
sudo apt install flatpak flatpak-builder
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.11 org.kde.Sdk//6.11 \
    org.freedesktop.Sdk.Extension.llvm21
```

(Check `runtime`/`runtime-version`/`sdk-extensions` in `.flatpak-manifest.json`
in case this fork has moved to a newer runtime since this was written.)

Build and install:

```bash
git clone <this repo> ~/GitHub/kdenlive-arynwood
cd ~/GitHub/kdenlive-arynwood
flatpak-builder --user --install --force-clean build-flatpak .flatpak-manifest.json
```

This is a full Kdenlive build (C++ + Qt + MLT) — expect it to take a while,
and expect to need to fix build errors that don't show up on the machine
that originally built it (that already happened once — see commit
"Fix flatpak build/runtime issues found during first build+launch"). Rebuild
with the same command after any manifest/source fix; `--force-clean` handles
incremental rebuilds fine, no need to wipe `build-flatpak/` yourself.

Run it with `flatpak run org.kde.kdenlive`, or just launch Kdenlive normally
if it's registered as a desktop app.

## 2. Verify the D-Bus scripting API is live

With Kdenlive running:

```bash
gdbus introspect --session --dest org.kde.kdenlive --object-path /MainWindow \
    | grep scriptGetProjectInfo
```

If that prints nothing, the D-Bus scripting patches didn't make it into the
build — check `git log` for the D-Bus scripting commits (`Add D-Bus scripting
API`, `Register MainWindow on D-Bus...`) actually being present on the branch
you built.

## 3. Set up mcp-kdenlive (MCP server)

```bash
cd mcp-kdenlive
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
systemd/install.sh   # installs + enables the mcp-kdenlive user service (port 8420)
```

`kdenlive-api` (the D-Bus Python wrapper) is installed automatically as a
local editable dependency — no separate step, no system D-Bus dev headers
needed (it shells out to `dbus-send`/`qdbus`/`gdbus`, whichever's on PATH).

## 4. Wire it into ArynCore

In `aryncore-mcp/mcp/config/mcp_servers.json`, make sure there's a `kdenlive`
entry pointing at `http://127.0.0.1:8420/mcp` (matches the port above). Pull
the model the tool-calling loop expects:

```bash
ollama pull qwen2.5-coder:14b
```

That's the whole dependency chain: flatpak Kdenlive → D-Bus → mcp-kdenlive
(systemd service, port 8420) → ArynCore's mcp_proxy → chat.py.
