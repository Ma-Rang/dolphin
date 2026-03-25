// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DolphinIPC: Dual-mode TCP protocol for external control of Dolphin.
//
// Enabled with --ipc_port <port> or Settings > Interface > Remote Control.
// Off by default.
//
// Protocol v1 — dual mode:
//   Lines starting with '{' are parsed as JSON.  All other lines use the
//   text protocol.
//
//   JSON request:  {"cmd": "command_name", ...params}
//   JSON response: {"ok": true, ...data} or {"error": "message"}
//
//   Text commands (telnet-friendly):
//     PING / VERSION / STATUS  — always available
//
//   JSON-only features:
//     set_config with "settings" array for batch writes (single save)
//
//   Events are always text: EVENT STARTED / EVENT STOPPED

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace Core
{
class System;
}

namespace DolphinIPC
{
// Maximum command line length (bytes). Lines exceeding this are rejected.
inline constexpr size_t MAX_COMMAND_LENGTH = 4096;

// Frontend-provided callbacks for operations that require main-thread dispatch.
// These differ between DolphinQt (QueueOnObject) and DolphinNoGUI (Core::Stop + BootCore).
struct FrontendCallbacks
{
  // Boot a game from file path. The path has already been base64-decoded.
  // Implementation should handle stop-then-boot if emulation is already running.
  std::function<void(const std::string& path)> boot_game;

  // Boot a NAND title by ID.
  // Implementation should handle stop-then-boot if emulation is already running.
  std::function<void(u64 title_id)> boot_nand;

  // Force-stop emulation.
  std::function<void()> force_stop;

  // Toggle fullscreen (Qt only; NoGUI returns ERR).
  std::function<std::string()> fullscreen_toggle;

  // Query fullscreen state (Qt only; NoGUI returns ERR).
  std::function<bool()> is_fullscreen;

  // Change disc without stopping emulation (requires CPUThreadGuard).
  std::function<std::string(const std::string& path)> change_disc;

  // Eject disc without stopping emulation.
  std::function<std::string()> eject_disc;

  // List games from the frontend's game cache (Qt only; NoGUI returns ERR).
  std::function<std::string()> list_games;

  // Exit the application entirely (stop emulation + close window/process).
  std::function<void()> exit_app;
};

// Callback interface for handling IPC commands.
// Return value is the response string sent to the client (e.g., "OK", "ERR reason").
struct CommandHandler
{
  std::function<std::string(const std::string& path)> on_boot;
  std::function<std::string(u64 title_id)> on_boot_nand;
  std::function<std::string()> on_stop;
  std::function<std::string()> on_status;
  std::function<std::string(const std::string& path)> on_install_wad;
  std::function<std::string(u64 title_id)> on_uninstall_title;
  std::function<std::string(u64 title_id)> on_is_title_installed;

  // Config read/write
  std::function<std::string(const std::string& system, const std::string& section,
                            const std::string& key)>
      on_get_config;
  std::function<std::string(const std::string& system, const std::string& section,
                            const std::string& key, const std::string& value)>
      on_set_config;
  std::function<std::string()> on_fullscreen_toggle;
  std::function<std::string()> on_get_fullscreen;
  std::function<std::string()> on_wiimote_sync;
  std::function<std::string()> on_wiimote_refresh;
  std::function<std::string(int channel, int device_type)> on_gc_change_device;
  std::function<std::string()> on_gc_adapter_status;

  // NAND queries (no emulation needed)
  std::function<std::string()> on_list_titles;

  // Save state handlers
  std::function<std::string(int slot)> on_save_state;
  std::function<std::string(int slot)> on_load_state;
  std::function<std::string()> on_list_save_states;
  std::function<std::string(const std::string& path)> on_save_state_file;
  std::function<std::string(const std::string& path)> on_load_state_file;

  // Screenshot
  std::function<std::string(const std::string& name)> on_screenshot;

  // System info
  std::function<std::string()> on_get_system_info;

  // Volume control
  std::function<std::string(int volume)> on_set_volume;
  std::function<std::string()> on_get_volume;
  std::function<std::string()> on_toggle_mute;

  // Config dump / schema
  std::function<std::string(const std::string& system_filter)> on_dump_config;
  std::function<std::string(const std::string& section)> on_get_config_schema;

  // Disc operations (frontend-dispatched)
  std::function<std::string(const std::string& path)> on_change_disc;
  std::function<std::string()> on_eject_disc;

  // Gecko port query
  std::function<std::string()> on_get_gecko_port;

  // Speed / frame control
  std::function<std::string(float speed)> on_set_speed;
  std::function<std::string()> on_get_speed;
  std::function<std::string()> on_frame_step;

  // Game library
  std::function<std::string()> on_list_games;
  std::function<std::string()> on_get_game_paths;

  // Pause / resume emulation
  std::function<std::string()> on_pause;
  std::function<std::string()> on_resume;

  // Exit the application
  std::function<std::string()> on_exit;

  // TAS movie commands
  std::function<std::string(const std::string& path)> on_play_movie;
  std::function<std::string()> on_record_movie;
  std::function<std::string()> on_stop_movie;
  std::function<std::string(const std::string& path)> on_save_movie;

  // Shared mutable state: last file path passed to BOOT (for JSON status response).
  // Updated by on_boot handler; read by the JSON status handler.
  std::shared_ptr<std::string> last_boot_path = std::make_shared<std::string>();
};

// Create a CommandHandler with all shared logic wired up.
// Only the frontend-specific callbacks (boot, stop, fullscreen) come from the caller.
// The returned handler is ready to pass to Server.
CommandHandler CreateHandlers(Core::System& system, FrontendCallbacks frontend);

// Decode a base64-encoded string. Returns empty string on failure.
std::string DecodeBase64(const std::string& encoded);

class Server
{
public:
  Server(u16 port, CommandHandler handler);
  ~Server();

  // Start the TCP listener thread. Non-blocking.
  void Start();

  // Stop the server and disconnect all clients.
  void Stop();

  // Send an event string to all connected clients.  Thread-safe.
  void BroadcastEvent(const std::string& event);

  bool IsRunning() const { return m_running.load(); }
  u16 GetPort() const { return m_port; }

private:
  void ServerThread();
  std::string HandleCommand(const std::string& line);

  u16 m_port;
  CommandHandler m_handler;
  std::atomic<bool> m_running{false};
  std::thread m_server_thread;

  // Connected clients, protected by m_clients_mutex
  struct ClientConnection;
  std::mutex m_clients_mutex;
  std::vector<std::unique_ptr<ClientConnection>> m_clients;

  // Auto-deregisters when Server is destroyed.
  Common::EventHook m_state_hook;
};

}  // namespace DolphinIPC
