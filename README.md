# Vita Control MCP

A PS Vita taiHEN user plugin and a desktop MCP server that let an LLM **see a
game's screen and press its buttons**. The Vita plugin serves authenticated TCP
on port 5000; the desktop bridge returns PNG images directly to the model and
provides timed buttons and analog-stick actions.

Implemented and cross-compiled with VitaSDK. **Smoke-tested on a physical Vita
in Papers, Please:** screenshots captured successfully, and timed D-pad and left
analog-stick inputs moved the title-menu selection. Other games, right-stick
controls, disconnect recovery, and extended reliability checks still need device
validation.

## Requirements

- A PS Vita with HENkaku/Ensō and taiHEN user plugins enabled.
- Vita and computer on the same trusted Wi-Fi network.
- Python 3.11+ on the computer, and an MCP client that supports image tool results.
- [VitaSDK](https://vitasdk.org/) with `taihen.h` and `libtaihen_stub.a` for building.
  The plugin uses [taiHEN import hooks](https://github.com/yifanlu/taiHEN/blob/master/USAGE.md)
  and the [VitaSDK controller APIs](https://docs.vitasdk.org/group__SceCtrlUser.html).

## Build

```sh
export VITASDK=/path/to/vitasdk
export PATH="$VITASDK/bin:$PATH"
cmake -S . -B build
cmake --build build -j
```

The result is `build/vita_control.suprx`. This is a **user plugin**, installed under
a game's title ID, not under `*KERNEL`. If your SDK lacks taiHEN development files,
install the `taihen` SDK package, or use the upstream
[taiHEN v0.11 SDK archive](https://github.com/yifanlu/taiHEN/releases/tag/v0.11)
(`include/` and `lib/` go in `$VITASDK/arm-vita-eabi/`).

## Install on the Vita

1. Install the host bridge and generate a token:

   ```sh
   python3 -m venv .venv
   .venv/bin/python -m pip install -e .
   .venv/bin/vita-control token --output token.txt
   ```

2. In VitaShell, create `ux0:data/vita-control/`. Copy `token.txt` into it. The
   token must be the same on both devices; it contains 32–64 printable ASCII
   characters, with an optional trailing newline. Missing or invalid tokens
   prevent the plugin from starting.

3. Copy `vita_control.suprx` to `ur0:tai/vita_control.suprx`.

4. Add the following to your **active** taiHEN config (usually
   `ur0:tai/config.txt`), replacing `TITLEID` with the game's actual title ID:

   ```text
   *TITLEID
   ur0:tai/vita_control.suprx
   ```

   If that section already exists, add the plugin path to the existing section.
   Start with one game. Do not use `*ALL`: multiple running applications would
   compete for the same port.

5. Reload taiHEN configuration or reboot as needed, then launch the game. Keep
   it in the foreground, with Wi-Fi connected. Find its IP address in Vita
   network settings or VitaShell.

## Check it manually

```sh
export VITA_HOST=192.168.1.50
export VITA_TOKEN_FILE="$PWD/token.txt"

.venv/bin/vita-control status
.venv/bin/vita-control screenshot --output screen.png
.venv/bin/vita-control press cross --duration-ms 100
.venv/bin/vita-control press up cross --duration-ms 300
.venv/bin/vita-control press --left-stick -1 0 --duration-ms 500
.venv/bin/vita-control release
```

Supported buttons: `cross`, `circle`, `square`, `triangle`, `up`, `down`, `left`,
`right`, `l`, `r`, `start`, `select`. Inputs last 1–5000 ms. Optional stick
coordinates are `[x, y]` in `[-1, 1]`: positive x is right and positive y is down.
`[0, 0]` centers a stick; omitting a stick preserves its physical input.

Physical buttons remain usable: injected buttons merge with them. Inputs expire
on the device's clock, including if the host stalls. Disconnects clear injected
input when detected. The CLI waits for each timed action and releases it before
returning. The game's sampling rate determines how many frames see a press;
100 ms is a useful starting point. Presses are not frame-exact.

## Connect an LLM through MCP

Add this server to your MCP client's configuration, using absolute paths:

```json
{
  "mcpServers": {
    "vita": {
      "command": "/absolute/path/vita-control-mcp/.venv/bin/vita-control-mcp",
      "env": {
        "VITA_HOST": "192.168.1.50",
        "VITA_TOKEN_FILE": "/absolute/path/vita-control-mcp/token.txt"
      }
    }
  }
}
```

Windows users can use the corresponding `.venv\\Scripts\\vita-control-mcp.exe`
path. `VITA_TOKEN` can supply the token directly instead of a file.
`VITA_PORT` defaults to 5000; changing it also requires changing `VC_PORT` in the
plugin and rebuilding.

The server exposes:

- `vita_status()` — connection check, hook availability, and button names.
- `vita_screenshot()` — captures the next submitted game frame and returns a PNG
  image as an MCP image result.
- `vita_press(buttons, duration_ms=100, left_stick=None, right_stick=None)` —
  presses buttons and/or moves sticks, waits, then releases. Use `buttons=[]`
  for stick-only movement.
- `vita_release()` — clears injected input in the session.

Example prompt: “Use the Vita tools to inspect the current game screen. Press
cross for 100 ms, then take another screenshot and describe what changed.”

Use one controller client at a time. The plugin serializes sessions; concurrent
tool calls wait behind the active one. `vita_release` does not preempt a timed
press in another connection; each press expires in at most five seconds.

## Scope and limitations

- This is a per-game user plugin. It does not control LiveArea, PS-button
  system overlays, touchscreens, motion sensors, PSP/PS1 emulation, or other
  processes. The PS/power buttons are deliberately outside the protocol.
- Hooks target the game's main module. Titles that poll controllers or submit
  display frames only from another module need additional hooks. Startup is
  refused if no supported controller import or display import can be hooked.
- Screenshots support the usual RGBA8 framebuffer, up to 960×544, respecting
  pitch and stripping alpha. Unsupported formats return an error. A capture
  waits for a new submitted frame for up to two seconds, so a suspended or
  non-rendering game cannot be captured. Screen overlays from other processes
  are not included.
- About 1.8 MiB of static buffers are reserved in the game process. Each requested
  capture adds a CPU copy on the display-submitting thread. PNG compression is
  done on the computer. This is on-demand observation, not video streaming.
- The plugin uses an existing game SceNet stack or initializes one if necessary.
  Games that later reinitialize or terminate SceNet may conflict with it. This
  needs testing per title. The plugin never terminates the game's network stack.
- Runtime unloading is refused while the game is alive to avoid removing code
  that another thread may be executing. Quit the game to unload; edit config
  and relaunch to disable or update it.
- The shared token authenticates access but does **not encrypt traffic**. Use
  a trusted LAN; do not expose TCP 5000 to the internet. Authentication and
  commands have bounded payloads, and socket operations have 15-second idle
  timeouts. The host applies a separate response deadline.

## Troubleshooting

Check `ux0:data/vita-control/plugin.log` for startup, hook, token, and socket
errors. If the directory is missing, the plugin cannot write the log.

- **Connection refused:** check plugin configuration/title ID, log, IP address,
  Wi-Fi, game foreground state, and whether another application owns port 5000.
- **Authentication failed:** recopy the same token to both sides and relaunch the
  game after replacing its token file.
- **No submitted frame:** return to the running game and retry. A game using
  another module for rendering needs more hooks.
- **Screenshot works but buttons do not:** the title may poll a different module
  or controller port. Only port 0 is injected. The status tool reports installed
  imports, not proof that the game's active input path uses those imports.
- **Sticks do nothing:** the game must already use analog sampling. The plugin
  preserves the game's sampling-mode choice.

## Development and verification

```sh
.venv/bin/python -m pip install -e '.[dev]'
.venv/bin/ruff check src
.venv/bin/ruff format --check src
```

The CI workflow lints the Python source and cross-compiles the `.suprx`.
See [the wire protocol](protocol.md) for other clients.
