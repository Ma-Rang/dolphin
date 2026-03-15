# IPC Server Overview

Dolphin includes an optional TCP server that allows external programs to control the emulator remotely. This enables use cases like custom frontends, Steam Deck quick-access panels, CI/CD test harnesses, accessibility tools, streaming overlays, and scripted automation.

The server speaks a line-delimited JSON protocol. All commands and responses are JSON objects, one per line. Events are broadcast as plain text lines.

## Enabling the server

The IPC server is **off by default**. It can be enabled three ways:

- **Command line**: Pass `--ipc_port <port>` when launching Dolphin. This works with both DolphinQt and DolphinNoGUI.
- **Settings GUI**: Go to Settings > Interface > Remote Control and check "Enable IPC Server". The port number is configurable in the same section.
- **Config file**: Add `IPCServerEnabled = True` and `IPCServerPort = 4830` under `[Interface]` in `Dolphin.ini`.

When `--ipc_port` is passed on the command line, it takes precedence over the GUI setting. The Remote Control controls in the settings window will be greyed out with a note indicating the override.

DolphinNoGUI can be started with only `--ipc_port` and no game. It will idle until an external program sends a `boot` command.

## How it works

Once enabled, Dolphin listens for TCP connections on the configured port (default 4830). Any number of clients can connect simultaneously. Each client can send commands and receive responses. All connected clients also receive push events when emulation state changes.

### Commands and responses

Commands are JSON objects, one per line, with a `cmd` field:

```json
{"cmd":"boot","path":"C:\\Games\\MarioKart.iso"}
{"ok":true}

{"cmd":"set_config","system":"WiiPad","section":"Wiimote1","key":"Source","value":"2"}
{"ok":true}

{"cmd":"status"}
{"ok":true,"state":"running","game_id":"RMCE01","title":"Mario Kart Wii","platform":"wii","has_path":true,"path":"C:\\Games\\MarioKart.iso"}
```

Errors return `{"error":"message"}` instead of `{"ok":true}`.

Batch config writes are supported: pass a `settings` array to `set_config` to change multiple values with a single disk write.

### Events

Events are plain text lines broadcast to all connected clients:

```
EVENT running
EVENT paused
EVENT stopping
EVENT uninitialized
```

Clients should silently ignore any event types they do not recognize, as new events may be added in future versions.

## What you can do with it

The IPC server exposes most of the controls available in Dolphin's GUI. The full command reference is in [IPC-Protocol.md](IPC-Protocol.md). Here is a summary of the available functionality:

**Emulation control** -- Boot games from file paths or NAND title IDs, stop/pause/resume emulation, query running status (including game metadata), toggle fullscreen.

**Save states** -- Save and load by slot (1-10) or by file path. List which slots are occupied.

**TAS movie** -- Play, record, stop, and save DTM input recordings. Enables scripted TAS playback for events, streaming, and testing.

**Configuration** -- Read and write any Dolphin setting using the same config system as the GUI. A schema endpoint provides type metadata (value ranges, enum options) for building dynamic settings UIs. Config system name aliases (`Main`/`Dolphin`, `WiiPad`/`Wiimote`, `GFX`/`Graphics`) are accepted so clients can use whichever form is more natural.

**Controller management** -- Switch Wiimote sources between None, Emulated, and Real. Toggle Bluetooth passthrough. Trigger Wiimote sync and refresh. Hot-swap GameCube controller port devices during emulation.

**Audio and speed** -- Set volume (0-100), toggle mute, change emulation speed, advance a single frame while paused.

**Disc operations** -- Hot-swap or eject discs during emulation. Parse disc metadata (game ID, title, region, platform) and extract banner files without starting emulation.

**NAND management** -- Install and uninstall WAD files, check whether a title is installed, list all installed NAND titles.

**System info** -- Query Dolphin's version, git branch/revision, video backend, and the USB Gecko emulation port.

**Game library** -- List all games in Dolphin's game cache with metadata (DolphinQt only). Query configured ISO search directories.

## Examples

### Quick test with netcat

```
$ echo '{"cmd":"version"}' | nc localhost 4830
{"ok":true,"version":1}
```

### Booting a game (Python)

```python
import socket, json

def ipc_cmd(sock, cmd):
    sock.sendall((json.dumps(cmd) + "\n").encode())
    return json.loads(sock.recv(4096).decode())

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("127.0.0.1", 4830))

# Check version
print(ipc_cmd(sock, {"cmd": "version"}))
# {"ok": true, "version": 1}

# Boot a game
print(ipc_cmd(sock, {"cmd": "boot", "path": "D:\\Games\\MarioKart.iso"}))
# {"ok": true}

# Wait for EVENT STARTED...

# Check status (includes game metadata when running)
print(ipc_cmd(sock, {"cmd": "status"}))
# {"ok": true, "state": "running", "game_id": "RMCE01", "title": "Mario Kart Wii",
#  "platform": "wii", "has_path": true, "path": "C:\\Games\\MarioKart.iso"}
```

### Switching Wiimote to Real mid-game

```python
# Set Wiimote 1 to Real (value 2)
ipc_cmd(sock, {
    "cmd": "set_config",
    "system": "WiiPad",
    "section": "Wiimote1",
    "key": "Source",
    "value": "2"
})

# Trigger a Bluetooth scan to find the controller
ipc_cmd(sock, {"cmd": "wiimote_refresh"})
```

### Batch config update

```python
# Change multiple settings in one write
ipc_cmd(sock, {
    "cmd": "set_config",
    "settings": [
        {"system": "WiiPad", "section": "Wiimote1", "key": "Source", "value": "2"},
        {"system": "WiiPad", "section": "Wiimote2", "key": "Source", "value": "0"},
        {"system": "Main", "section": "BluetoothPassthrough", "key": "Enabled", "value": "True"}
    ]
})
```

### Save state management

```python
# Save to slot 1
ipc_cmd(sock, {"cmd": "save_state", "slot": 1})

# List all slots
result = ipc_cmd(sock, {"cmd": "list_save_states"})
for slot in result["slots"]:
    status = "empty" if slot["empty"] else slot.get("info", "occupied")
    print(f"Slot {slot['slot']}: {status}")
```

## Known implementations

The following projects are known to use or target the Dolphin IPC protocol:

*(None published yet. If you have built a tool or plugin that uses this protocol, consider adding it here.)*

<!-- Example format:
- **Project Name** -- Brief description. [Link](https://example.com)
-->

## Protocol reference

The full protocol specification, including all commands, argument formats, response formats, config key tables, and error codes, is in [IPC-Protocol.md](IPC-Protocol.md).
