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

Run it with `flatpak run com.arynwood.Cutroom`, or just launch Arynwood Cutroom
from your application menu.

Earlier alpha builds of this fork installed as `org.kde.kdenlive`, the same ID as
Kdenlive itself. If you built one, it is still installed under that name and its
settings are separate from Cutroom's. Once the new build works, remove the old one
with `flatpak uninstall --user org.kde.kdenlive`. (Skip this if you also use the
real Kdenlive from Flathub under that ID.) Cutroom now has its own ID, so it can be
installed beside Kdenlive.

The tool server (step 3) finds the editor by its D-Bus name, and Cutroom's name changed
along with its ID. A tool server that was already running still looks only for the old
name and will answer "Kdenlive isn't running", so restart it once after
updating: `systemctl --user restart mcp-kdenlive`. (The updated client tries Cutroom's
name first and still finds a Kdenlive fork that uses the old one.)

## 2. Verify the D-Bus scripting API is live

With Kdenlive running:

```bash
gdbus introspect --session --dest com.arynwood.Cutroom --object-path /MainWindow \
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

## 5. The Cutroom Assistant panel (chat inside the editor)

The panel is built into this fork, so there is nothing more to install into
Cutroom itself. It needs the two things it talks to:

1. **The tool server from step 3**, running:
   `systemctl --user status mcp-kdenlive` (it should be `active`).
2. **A model that supports tool calling.** Only models Ollama lists with the
   `tools` capability work; `ollama show <model>` prints its capabilities, and a
   model without it makes Ollama answer `does not support tools`, which the panel
   shows. Any such model will do, and nothing here is tied to one maker. With
   Ollama running, for example:

   ```bash
   ollama pull qwen2.5-coder:14b     # what the MCP README recommends
   ```

   **Expect to supervise it.** Models small enough for a 12 GB graphics card are
   unreliable at multi-step edits. In testing on "add all the photos in the bin to
   the timeline", the best of the models tried got it exactly right in about one
   run in three; others answered in prose without calling any tool, invented clip
   ids, or repeated actions. That is why changes ask for your approval by default
   (below), and why a hosted model, or a larger local one, will do noticeably
   better if you have access to one.

Then click **Launch Assistant**, the button with the Arynwood tree in the top right
corner of the window (left of the workspace tabs). **View → Cutroom Assistant** does
the same. It opens as a tab beside the Clip Monitor and Library. (If the window is
narrower than about 1250 px, the last menus fold into the `»` menu to make room for
the button.) Under the model picker, one green dot means the model server is
reachable and another that the tool server is (with its tool count); hover over
the line to see why if either is red. Pick a model and describe an edit in plain
language.

- **Where things run.** The Cutroom flatpak has network access, so `127.0.0.1`
  reaches Ollama and the tool server on the host. Addresses are in the gear
  button's settings (defaults: Ollama `http://127.0.0.1:11434`, tools
  `http://127.0.0.1:8420/mcp`).
- **A hosted API instead of Ollama.** Gear button → Provider "OpenAI-compatible
  API", then the API root (for example `https://api.openai.com/v1`) and a key.
  This works with anything that speaks the OpenAI chat-completions dialect, such
  as OpenRouter, LM Studio or llama.cpp's server. With a remote service, your
  messages and the tool results (project, clip and file names) are sent to it;
  the panel says so in its status line. Keep the model on Ollama if the footage
  and project details must stay on this machine. The key is stored in
  Kdenlive's config file obfuscated, not encrypted.
- **Context size.** The default 16384 tokens is a starting point for Ollama; the
  panel sends only the tools that fit each request so small windows work. See
  `src/aichat/README.md` for why, and for the "All tools" option.
- **Changes ask first.** By default every tool that changes the project waits for
  your Allow or Deny, and reading or looking around never asks. In the gear
  button's settings you can relax that to only the actions that can lose work
  or overwrite files (opening, loading or creating a project, restoring a
  checkpoint, rendering, cancelling a render job, exporting subtitles), or
  turn asking off. Whatever you choose, a single request is limited to 12 tool
  calls per model reply and 40 in all, and an identical change is not run more
  than twice. The assistant edits the open project directly and not every action
  can be undone, so save a copy of anything important first.
