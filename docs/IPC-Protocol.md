# Dolphin IPC Protocol Reference

Complete command reference for Dolphin's IPC server. For a high-level overview, setup instructions, and usage examples, see [IPC-Overview.md](IPC-Overview.md).

The server is off by default. Enable it with any of:

- **Command line**: `--ipc_port 4830` (works with both DolphinQt and DolphinNoGUI)
- **Settings GUI**: Settings > Interface > Remote Control > Enable IPC Server
- **Config file**: Set `IPCServerEnabled = True` and `IPCServerPort = 4830` under `[Interface]` in `Dolphin.ini`

When `--ipc_port` is used, it overrides the GUI setting.

## Protocol overview

The protocol is line-delimited (`\n`). All communication uses JSON.

- **Request**: `{"cmd":"command_name", ...}\n`
- **Response**: `{"ok":true, ...}\n` or `{"error":"message"}\n`

Events are plain text lines: `EVENT running`, `EVENT stopped`, etc. Clients should ignore unknown event types.

Path arguments are plain UTF-8 strings. Maximum line length is 4096 bytes.

Multiple clients may connect simultaneously. All connected clients receive event broadcasts.

## Command listing

The following tables document every command. The "Requires" column indicates whether running emulation is needed. Commands marked "Qt only" return an error in DolphinNoGUI.

### Connection

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `ping` | none | `{"ok":true}` | Keepalive. |
| `version` | none | `{"ok":true,"version":1}` | Protocol version number. |

### Emulation control

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `status` | none | `{"ok":true,"state":"running","game_id":"GALE01","title":"...","platform":"gamecube","has_path":true,"path":"..."}` | Extra fields present only when state is `running`. State is one of: `uninitialized`, `starting`, `running`, `paused`, `stopping`. |
| `boot` | `path` (string) | `{"ok":true}` | Stops current emulation first if running. Same code path as double-clicking a game in the GUI. |
| `boot_nand` | `title_id` (hex string) | `{"ok":true}` | Title ID is 16 hex digits, e.g. `0000000100000002` for the Wii System Menu. |
| `stop` | none | `{"ok":true}` | Requires running. |
| `pause` | none | `{"ok":true}` | Requires running. |
| `resume` | none | `{"ok":true}` | Requires paused. |
| `fullscreen_toggle` | none | `{"ok":true}` | Qt only. |

### Save states

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `save_state` | `slot` (1-10) | `{"ok":true}` | Requires running. |
| `load_state` | `slot` (1-10) | `{"ok":true}` | Requires running. |
| `list_save_states` | none | `{"ok":true,"slots":[...]}` | |
| `save_state_file` | `path` (string) | `{"ok":true}` | Requires running. |
| `load_state_file` | `path` (string) | `{"ok":true}` | Requires running. |

### Screenshot

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `screenshot` | `name` (optional, no path or extension) | `{"ok":true}` | Requires running. |

### Configuration

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `get_config` | `system`, `section`, `key` | `{"ok":true,"value":"..."}` | |
| `set_config` | `system`, `section`, `key`, `value` | `{"ok":true}` | Writes to Base layer, persists to INI. |
| `set_config` | `settings` (array of objects) | `{"ok":true}` | Batch mode. Writes all values then saves once. |
| `dump_config` | `system` (optional filter) | `{"ok":true,"config":{...}}` | |
| `get_config_schema` | `section` | `{"ok":true,"section":"...","settings":[...]}` | See **Config schema sections** below. |

### Volume and speed

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `set_volume` | `volume` (int, 0-100) | `{"ok":true}` | |
| `get_volume` | none | `{"ok":true,"volume":50}` | |
| `toggle_mute` | none | `{"ok":true,"muted":true}` | |
| `set_speed` | `speed` (float) | `{"ok":true}` | 0 = unlimited, 1.0 = normal, 2.0 = double. |
| `get_speed` | none | `{"ok":true,"speed":1.0}` | |
| `frame_step` | none | `{"ok":true}` | Requires paused emulation. |

### Controllers

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `wiimote_sync` | none | `{"ok":true}` | Triggers Bluetooth sync (requires BT passthrough and running emulation). |
| `wiimote_refresh` | none | `{"ok":true}` | Scans for real Wiimotes. |
| `gc_change_device` | `channel` (0-3), `device_type` (int) | `{"ok":true}` | Hot-swaps a GameCube controller port. See **SIDevices** below. |
| `gc_adapter_status` | none | `{"ok":true,"detected":true}` | |

### Disc operations

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `change_disc` | `path` (string) | `{"ok":true}` | Requires running. |
| `eject_disc` | none | `{"ok":true}` | Requires running. |

### NAND management

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `install_wad` | `path` (string) | `{"ok":true}` | Must not be called during emulation. |
| `uninstall_title` | `title_id` (hex string) | `{"ok":true}` | |
| `is_title_installed` | `title_id` (hex string) | `{"ok":true,"installed":true}` | |
| `list_titles` | none | `{"ok":true,"titles":["..."]}` | |

### System information

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `get_system_info` | none | `{"ok":true,"version":"...","branch":"...","revision":"...","os":"...","ipc_version":1,"backend":"..."}` | |
| `get_gecko_port` | none | `{"ok":true,"active":true,"port":55020}` | Reports the USB Gecko emulation TCP port. |

### Game library

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `list_games` | none | `{"ok":true,"games":[...]}` | Qt only. |
| `get_game_paths` | none | `{"ok":true,"paths":["..."]}` | |

### TAS movie

| `cmd` | Arguments | Response | Notes |
|---|---|---|---|
| `play_movie` | `path` (string, DTM file) | `{"ok":true}` | Loads movie and begins playback. Sets read-only mode. Stop active movies first. |
| `record_movie` | none | `{"ok":true}` | Begins recording input. Reads controller config from current settings. Stop active movies first. |
| `stop_movie` | none | `{"ok":true}` | Stops recording or playback. |
| `save_movie` | `path` (string, DTM file) | `{"ok":true}` | Saves the current recording to a file. Requires active recording or playback. |

## Events

Events are broadcast to all connected clients as plain text lines.

| Event | Meaning |
|---|---|
| `EVENT running` | Emulation has started or resumed. |
| `EVENT paused` | Emulation has been paused. |
| `EVENT stopping` | Emulation is shutting down. |
| `EVENT uninitialized` | Emulation has fully stopped. |
| `EVENT starting` | Emulation is initializing. |

Clients should ignore unrecognized event types for forward compatibility.

## Config system name aliases

Dolphin's internal config system names differ from the names used in documentation and the GUI. Both forms are accepted by `GET_CONFIG`, `SET_CONFIG`, and `DUMP_CONFIG`.

| Alias | Canonical (internal) |
|---|---|
| Main | Dolphin |
| WiiPad | Wiimote |
| GFX | Graphics |

The full list of canonical system names: `Dolphin`, `SYSCONF`, `Wiimote`, `GCPad`, `GCKeyboard`, `Graphics`, `Logger`, `DualShockUDPClient`, `FreeLook`, `Session`, `GameSettingsOnly`, `Achievements`.

## Controller config reference

### Wiimote source

| System | Section | Key | Values |
|---|---|---|---|
| WiiPad | Wiimote1 through Wiimote4 | Source | 0 = None, 1 = Emulated, 2 = Real |

### Bluetooth passthrough

| System | Section | Key | Values |
|---|---|---|---|
| Main | BluetoothPassthrough | Enabled | True / False |

### GameCube port type (SIDevices)

| System | Section | Key | Values |
|---|---|---|---|
| Main | Core | SIDevice0 through SIDevice3 | See table below |

| Value | Device |
|---|---|
| 0 | None |
| 5 | GBA (TCP) |
| 6 | Standard Controller |
| 7 | Keyboard Controller |
| 8 | Steering Wheel |
| 9 | Dance Mat |
| 10 | DK Bongos |
| 11 | Triforce Baseboard |
| 12 | GameCube Controller Adapter (USB) |
| 13 | GBA (Integrated) |

## Config schema sections

The `GET_CONFIG_SCHEMA` command returns type metadata for config keys in a given section. The response is always JSON, even in text mode. Each setting includes its system, section, key, type (`bool`, `int`, `float`, `string`, or `enum`), current value, default value, and for enums, the list of valid options.

| Section | Contents |
|---|---|
| `controllers` | GC port types (SIDevice0-3), Wiimote sources (1-4), BT passthrough |
| `graphics` | Video backend, internal resolution, aspect ratio, VSync, MSAA, FPS display |
| `audio` | Audio backend, volume, DSP JIT, audio stretch, mute |
| `paths` | ISO search directories |
| `core` | CPU core type, dual core, emulation speed, overclock, MMU, DCBZ, FPRF, sync GPU |

## Error responses

All errors return `{"error": "message"}`. The `error` field contains a human-readable description. Common errors:

| Error | Meaning |
|---|---|
| `Emulation not running` | Command requires active emulation. |
| `Emulation not paused` | Resume was called but emulation is not paused. |
| `Not available in headless mode` | Command is only supported in DolphinQt. |
| `Slot must be 1-10` | Save state slot out of range. |
| `Missing 'path' parameter` | Required path argument was not provided. |
| `Unknown command: ...` | Unrecognized command name. |
| `Movie already active (stop it first)` | A TAS movie is already playing or recording. |
| `No movie active` | Stop/save was called with no active movie. |

## Example session

```json
{"cmd":"version"}
{"ok":true,"version":1}

{"cmd":"status"}
{"ok":true,"state":"uninitialized"}

{"cmd":"boot","path":"C:\\Games\\MarioKart.iso"}
{"ok":true}
EVENT running

{"cmd":"status"}
{"ok":true,"state":"running","game_id":"RMCE01","title":"Mario Kart Wii","platform":"wii","has_path":true,"path":"C:\\Games\\MarioKart.iso"}

{"cmd":"pause"}
{"ok":true}
EVENT paused

{"cmd":"resume"}
{"ok":true}
EVENT running

{"cmd":"set_config","settings":[{"system":"WiiPad","section":"Wiimote1","key":"Source","value":"2"}]}
{"ok":true}

{"cmd":"save_state","slot":1}
{"ok":true}

{"cmd":"record_movie"}
{"ok":true}

{"cmd":"save_movie","path":"C:\\Movies\\my_run.dtm"}
{"ok":true}

{"cmd":"stop_movie"}
{"ok":true}

{"cmd":"stop"}
{"ok":true}
EVENT stopping
EVENT uninitialized
```

# References

- [IPC Overview](IPC-Overview.md)
- [DolphinIPC.h](/Source/Core/Core/DolphinIPC.h)
- [DolphinIPC.cpp](/Source/Core/Core/DolphinIPC.cpp)
