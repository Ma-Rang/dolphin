// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/DolphinIPC.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SFML/Network.hpp>
#include <fmt/format.h>
#include <mbedtls/base64.h>
#include <picojson.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Version.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Movie.h"
#include "Core/HW/EXI/EXI_DeviceGecko.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/IOS.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "InputCommon/GCAdapter.h"

namespace DolphinIPC
{
static constexpr int PROTOCOL_VERSION = 1;

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::string DecodeBase64(const std::string& encoded)
{
  if (encoded.empty())
    return {};

  // Determine decoded length.
  size_t decoded_len = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decoded_len,
                                  reinterpret_cast<const unsigned char*>(encoded.data()),
                                  encoded.size());
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || decoded_len == 0)
    return {};

  std::vector<unsigned char> buf(decoded_len);
  ret = mbedtls_base64_decode(buf.data(), buf.size(), &decoded_len,
                              reinterpret_cast<const unsigned char*>(encoded.data()),
                              encoded.size());
  if (ret != 0)
    return {};

  return std::string(reinterpret_cast<char*>(buf.data()), decoded_len);
}

static std::string EncodeBase64(const u8* data, size_t len)
{
  size_t b64_len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64_len, data, len);

  std::vector<unsigned char> b64_buf(b64_len);
  int ret = mbedtls_base64_encode(b64_buf.data(), b64_buf.size(), &b64_len, data, len);
  if (ret != 0)
    return {};

  return std::string(reinterpret_cast<char*>(b64_buf.data()), b64_len);
}

// Non-throwing numeric parsers (replace std::stoi/stof/stoull).
static std::optional<int> ParseInt(const std::string& s)
{
  int value = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || ptr != s.data() + s.size())
    return std::nullopt;
  return value;
}

static std::optional<int> ParseInt(const char* begin, const char* end)
{
  int value = 0;
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end)
    return std::nullopt;
  return value;
}

static std::optional<u64> ParseHexU64(const std::string& s)
{
  u64 value = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
  if (ec != std::errc{} || ptr != s.data() + s.size())
    return std::nullopt;
  return value;
}

static std::optional<float> ParseFloat(const std::string& s)
{
  // std::from_chars for float isn't available on all toolchains, so use strtof.
  if (s.empty())
    return std::nullopt;
  char* end = nullptr;
  float value = std::strtof(s.c_str(), &end);
  if (end != s.c_str() + s.size())
    return std::nullopt;
  return value;
}

static std::optional<double> ParseDouble(const std::string& s)
{
  if (s.empty())
    return std::nullopt;
  char* end = nullptr;
  double value = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size())
    return std::nullopt;
  return value;
}

// Split string on spaces into at most max_parts pieces.
// The last piece contains everything remaining (preserves spaces in values).
static std::vector<std::string> SplitArgs(const std::string& str, int max_parts)
{
  std::vector<std::string> parts;
  size_t start = 0;
  for (int i = 1; i < max_parts && start < str.size(); ++i)
  {
    size_t space = str.find(' ', start);
    if (space == std::string::npos)
      break;
    parts.push_back(str.substr(start, space - start));
    start = space + 1;
  }
  if (start < str.size())
    parts.push_back(str.substr(start));
  return parts;
}

// Resolve IPC system name to Config::System.
// Accepts both Dolphin's internal names (Dolphin, Wiimote, GCPad, Graphics, ...)
// and friendly aliases (Main, WiiPad, GFX) used by the IPC protocol docs.
static std::optional<Config::System> ResolveConfigSystem(const std::string& name)
{
  auto system = Config::GetSystemFromName(name);
  if (system)
    return system;
  static const std::map<std::string, std::string> aliases = {
      {"Main", "Dolphin"},
      {"WiiPad", "Wiimote"},
      {"GFX", "Graphics"},
  };
  auto it = aliases.find(name);
  if (it != aliases.end())
    return Config::GetSystemFromName(it->second);
  return {};
}

// ---------------------------------------------------------------------------
// Shared handler implementations
// ---------------------------------------------------------------------------

// Returns the current Core::State as a lowercase string via CoreStateToString().
// CoreStateToString() is defined near Server::Start() — source enum: Core::State in Core/Core.h
static const char* CoreStateToString(Core::State state);

static const char* GetCoreStateString(Core::System& system)
{
  return CoreStateToString(Core::GetState(system));
}

static std::string HandleInstallWAD(Core::System& system, const std::string& b64path)
{
  if (!Core::IsUninitialized(system))
    return "ERR Emulation is running (stop emulation first)";
  const std::string path = DecodeBase64(b64path);
  if (path.empty())
    return "ERR Invalid base64 path";
  if (WiiUtils::InstallWAD(path))
    return "OK INSTALLED";
  return "ERR WAD installation failed";
}

static std::string HandleUninstallTitle(Core::System& system, u64 title_id)
{
  if (!Core::IsUninitialized(system))
    return "ERR Emulation is running (stop emulation first)";
  if (WiiUtils::UninstallTitle(title_id))
    return "OK";
  return "ERR Title uninstallation failed";
}

static std::string HandleIsTitleInstalled(Core::System& system, u64 title_id)
{
  if (!Core::IsUninitialized(system))
    return "ERR Emulation is running (stop emulation first)";
  return WiiUtils::IsTitleInstalled(title_id) ? "OK TRUE" : "OK FALSE";
}

static std::string HandleListTitles(Core::System& system)
{
  if (!Core::IsUninitialized(system))
    return "ERR Emulation is running (stop emulation first)";
  IOS::HLE::Kernel ios;
  const std::vector<u64> titles = ios.GetESCore().GetInstalledTitles();
  if (titles.empty())
    return "OK";
  std::string result = "OK";
  for (const u64 tid : titles)
    result += fmt::format(" {:016X}", tid);
  return result;
}

static std::string HandleGetConfig(const std::string& sys_name, const std::string& section,
                                   const std::string& key)
{
  auto system = ResolveConfigSystem(sys_name);
  if (!system)
    return "ERR Invalid system name";
  auto value = Config::GetAsString(Config::Location{*system, section, key});
  if (!value)
    return "ERR Config key not found";
  return "OK " + *value;
}

static std::string HandleSetConfig(const std::string& sys_name, const std::string& section,
                                   const std::string& key, const std::string& value)
{
  auto system = ResolveConfigSystem(sys_name);
  if (!system)
    return "ERR Invalid system name";
  Config::GetLayer(Config::LayerType::Base)
      ->Set(Config::Location{*system, section, key}, value);
  Config::OnConfigChanged();
  Config::Save();
  return "OK";
}

static std::string HandleWiimoteSync(Core::System& system)
{
  if (!Core::IsRunning(system))
    return "ERR Not running";
  auto device = WiiUtils::GetBluetoothRealDevice();
  if (!device)
    return "ERR BT passthrough not active";
  Core::QueueHostJob([device](Core::System&) { device->TriggerSyncButtonPressedEvent(); });
  return "OK";
}

static std::string HandleWiimoteRefresh()
{
  Core::QueueHostJob([](Core::System&) { WiimoteReal::Refresh(); });
  return "OK";
}

static std::string HandleGCChangeDevice(Core::System& system, int channel, int device_type)
{
  if (!Core::IsRunning(system))
    return "ERR Not running";
  if (channel < 0 || channel > 3)
    return "ERR Invalid channel (0-3)";
  Core::QueueHostJob([&system, channel, device_type](Core::System&) {
    system.GetSerialInterface().ChangeDevice(
        static_cast<SerialInterface::SIDevices>(device_type), channel);
  });
  return "OK";
}

static std::string HandleGCAdapterStatus()
{
  const char* error_message = nullptr;
  bool detected = GCAdapter::IsDetected(&error_message);
  if (detected)
    return "OK DETECTED";
  if (error_message)
    return fmt::format("OK NOT_DETECTED {}", error_message);
  return "OK NOT_DETECTED";
}

static std::string HandleScanGame(const std::string& b64path)
{
  const std::string path = DecodeBase64(b64path);
  if (path.empty())
    return "ERR Invalid base64 path";

  auto volume = DiscIO::CreateVolume(path);
  if (!volume)
    return "ERR Could not open disc image";

  const DiscIO::Partition partition = volume->GetGamePartition();
  const std::string game_id = volume->GetGameID(partition);
  const std::string internal_name = volume->GetInternalName(partition);
  const std::string maker_id = volume->GetMakerID(partition);
  const DiscIO::Region region = volume->GetRegion();
  const DiscIO::Platform platform = volume->GetVolumeType();
  const std::optional<u8> disc_number = volume->GetDiscNumber(partition);
  const std::optional<u16> revision = volume->GetRevision(partition);

  // Try to get a localized title from banner; fall back to internal name.
  std::string title = internal_name;
  const auto long_names = volume->GetLongNames();
  if (!long_names.empty())
  {
    auto it = long_names.find(DiscIO::Language::English);
    if (it == long_names.end())
      it = long_names.begin();
    if (!it->second.empty())
      title = it->second;
  }

  // Resolve maker ID to a human-readable company name.
  const std::string& maker = DiscIO::GetCompanyFromID(maker_id);

  const char* region_str = "UNKNOWN";
  switch (region)
  {
  case DiscIO::Region::NTSC_J:
    region_str = "NTSC_J";
    break;
  case DiscIO::Region::NTSC_U:
    region_str = "NTSC_U";
    break;
  case DiscIO::Region::PAL:
    region_str = "PAL";
    break;
  case DiscIO::Region::NTSC_K:
    region_str = "NTSC_K";
    break;
  default:
    break;
  }

  const char* platform_str = "UNKNOWN";
  switch (platform)
  {
  case DiscIO::Platform::GameCubeDisc:
    platform_str = "GC";
    break;
  case DiscIO::Platform::WiiDisc:
    platform_str = "WII";
    break;
  case DiscIO::Platform::WiiWAD:
    platform_str = "WAD";
    break;
  default:
    break;
  }

  return fmt::format(
      "OK GAME_ID:{} TITLE:{} REGION:{} PLATFORM:{} DISC_NUMBER:{} REVISION:{} MAKER_ID:{} "
      "MAKER:{}",
      game_id, title, region_str, platform_str, disc_number.value_or(0),
      revision.value_or(0), maker_id, maker);
}

static std::string HandleGetBanner(const std::string& b64path)
{
  const std::string path = DecodeBase64(b64path);
  if (path.empty())
    return "ERR Invalid base64 path";

  auto volume = DiscIO::CreateVolume(path);
  if (!volume)
    return "ERR Could not open disc image";

  const DiscIO::Partition partition = volume->GetGamePartition();
  const DiscIO::FileSystem* fs = volume->GetFileSystem(partition);
  if (!fs || !fs->IsValid())
    return "ERR Could not read disc filesystem";

  auto file_info = fs->FindFileInfo("opening.bnr");
  if (!file_info)
    return "ERR No banner found";

  const u32 file_size = file_info->GetSize();
  if (file_size == 0)
    return "ERR Banner file is empty";

  // Read banner data from disc.
  std::vector<u8> banner_data(file_size);
  const u64 bytes_read =
      DiscIO::ReadFile(*volume, partition, file_info.get(), banner_data.data(), file_size);
  if (bytes_read != file_size)
    return "ERR Failed to read banner data";

  std::string b64 = EncodeBase64(banner_data.data(), banner_data.size());
  if (b64.empty())
    return "ERR Base64 encoding failed";

  return "OK " + b64;
}

// ---------------------------------------------------------------------------
// v1 expansion handlers — save states, screenshot, system info, volume,
// config dump/schema, gecko port, speed/frame, game paths
// ---------------------------------------------------------------------------

static std::string HandleSaveState(Core::System& system, int slot)
{
  if (slot < 1 || slot > 10)
    return "ERR Slot must be 1-10";
  if (!Core::IsRunning(system))
    return "ERR Emulation not running";
  State::Save(system, slot);
  return "OK";
}

static std::string HandleLoadState(Core::System& system, int slot)
{
  if (slot < 1 || slot > 10)
    return "ERR Slot must be 1-10";
  if (!Core::IsRunning(system))
    return "ERR Emulation not running";
  State::Load(system, slot);
  return "OK";
}

static std::string HandleListSaveStates()
{
  std::string result = "OK";
  for (int slot = 1; slot <= 10; ++slot)
  {
    const std::string info = State::GetInfoStringOfSlot(slot, false);
    result += fmt::format("\nSLOT {} {}", slot, info.empty() ? "EMPTY" : info);
  }
  return result;
}

static std::string HandleSaveStateFile(Core::System& system, const std::string& b64path)
{
  const std::string path = DecodeBase64(b64path);
  if (path.empty())
    return "ERR Invalid base64 path";
  if (!Core::IsRunning(system))
    return "ERR Emulation not running";
  State::SaveAs(system, path);
  return "OK";
}

static std::string HandleLoadStateFile(Core::System& system, const std::string& b64path)
{
  const std::string path = DecodeBase64(b64path);
  if (path.empty())
    return "ERR Invalid base64 path";
  if (!Core::IsRunning(system))
    return "ERR Emulation not running";
  State::LoadAs(system, path);
  return "OK";
}

static std::string HandleScreenshot(const std::string& name)
{
  if (name.empty())
    Core::SaveScreenShot();
  else
    Core::SaveScreenShot(name);
  return "OK";
}

static std::string HandleGetSystemInfo()
{
  std::string os_name;
#if defined(_WIN32)
  os_name = "windows";
#elif defined(__APPLE__)
  os_name = "macos";
#elif defined(__linux__)
  os_name = "linux";
#elif defined(ANDROID)
  os_name = "android";
#else
  os_name = "unknown";
#endif

  const std::string backend = Config::Get(Config::MAIN_GFX_BACKEND);
  const std::string user_dir = File::GetUserPath(D_USER_IDX);
  return fmt::format(
      "OK VERSION:{} BRANCH:{} REVISION:{} OS:{} IPC_VERSION:{} BACKEND:{} USER_DIR:{}",
      Common::GetScmDescStr(), Common::GetScmBranchStr(), Common::GetScmRevStr(), os_name,
      PROTOCOL_VERSION, backend, user_dir);
}

static std::string HandleSetVolume(Core::System& system, int volume)
{
  if (volume < 0 || volume > 100)
    return "ERR Volume must be 0-100";
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_VOLUME, volume);
  AudioCommon::UpdateSoundStream(system);
  return "OK";
}

static std::string HandleGetVolume()
{
  return fmt::format("OK {}", Config::Get(Config::MAIN_AUDIO_VOLUME));
}

static std::string HandleToggleMute(Core::System& system)
{
  const bool new_muted = !Config::Get(Config::MAIN_AUDIO_MUTED);
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_MUTED, new_muted);
  AudioCommon::UpdateSoundStream(system);
  return fmt::format("OK {}", new_muted ? "MUTED" : "UNMUTED");
}

static std::string HandleDumpConfig(const std::string& system_filter)
{
  std::optional<Config::System> filter_system;
  if (!system_filter.empty())
  {
    filter_system = ResolveConfigSystem(system_filter);
    if (!filter_system)
      return "ERR Invalid system name";
  }

  auto layer = Config::GetLayer(Config::LayerType::Base);
  if (!layer)
    return "ERR Config layer not available";

  std::string result = "OK";
  for (const auto& [location, value] : layer->GetLayerMap())
  {
    if (!value)
      continue;

    if (filter_system && location.system != *filter_system)
      continue;

    const std::string sys_name = Config::GetSystemName(location.system);
    result += fmt::format("\n{}.{}.{}={}", sys_name, location.section, location.key, *value);
  }
  return result;
}

static std::string HandleGetGeckoPort()
{
  if (ExpansionInterface::GeckoSockServer::IsServerRunning())
    return fmt::format("OK {}", ExpansionInterface::GeckoSockServer::GetServerPort());
  return "OK INACTIVE";
}

static std::string HandleSetSpeed(float speed)
{
  if (speed < 0.0f)
    return "ERR Speed must be >= 0 (0 = unlimited)";
  Config::SetBaseOrCurrent(Config::MAIN_EMULATION_SPEED, speed);
  return "OK";
}

static std::string HandleGetSpeed()
{
  return fmt::format("OK {:.2f}", Config::Get(Config::MAIN_EMULATION_SPEED));
}

static std::string HandleFrameStep(Core::System& system)
{
  if (!Core::IsRunning(system))
    return "ERR Emulation not running";
  Core::DoFrameStep(system);
  return "OK";
}

static std::string HandleGetGamePaths()
{
  const auto paths = Config::GetIsoPaths();
  std::string result = "OK";
  for (const auto& p : paths)
    result += "\n" + p;
  return result;
}

// Read a config value, returning empty string on error.
static std::string GetConfigValue(const char* sys, const char* sec, const char* key)
{
  const std::string result = HandleGetConfig(sys, sec, key);
  if (result.size() > 3 && result.substr(0, 3) == "OK ")
    return result.substr(3);
  return "";
}

// Config schema — hardcoded metadata for known config sections.
static std::string HandleGetConfigSchema(const std::string& section)
{
  // Build a JSON response with schema information for the requested section.
  picojson::array settings;

  auto add_bool = [&](const char* sys, const char* sec, const char* key, bool default_val) {
    picojson::object s;
    s.emplace("system", picojson::value(std::string(sys)));
    s.emplace("section", picojson::value(std::string(sec)));
    s.emplace("key", picojson::value(std::string(key)));
    s.emplace("type", picojson::value(std::string("bool")));
    s.emplace("value", picojson::value(GetConfigValue(sys, sec, key)));
    s.emplace("default", picojson::value(default_val ? std::string("True") : std::string("False")));
    settings.emplace_back(picojson::value(s));
  };

  auto add_int = [&](const char* sys, const char* sec, const char* key, int default_val, int min_val,
                      int max_val) {
    picojson::object s;
    s.emplace("system", picojson::value(std::string(sys)));
    s.emplace("section", picojson::value(std::string(sec)));
    s.emplace("key", picojson::value(std::string(key)));
    s.emplace("type", picojson::value(std::string("int")));
    s.emplace("value", picojson::value(GetConfigValue(sys, sec, key)));
    s.emplace("default", picojson::value(static_cast<double>(default_val)));
    s.emplace("min", picojson::value(static_cast<double>(min_val)));
    s.emplace("max", picojson::value(static_cast<double>(max_val)));
    settings.emplace_back(picojson::value(s));
  };

  auto add_float = [&](const char* sys, const char* sec, const char* key, float default_val,
                        float min_val, float max_val) {
    picojson::object s;
    s.emplace("system", picojson::value(std::string(sys)));
    s.emplace("section", picojson::value(std::string(sec)));
    s.emplace("key", picojson::value(std::string(key)));
    s.emplace("type", picojson::value(std::string("float")));
    s.emplace("value", picojson::value(GetConfigValue(sys, sec, key)));
    s.emplace("default", picojson::value(static_cast<double>(default_val)));
    s.emplace("min", picojson::value(static_cast<double>(min_val)));
    s.emplace("max", picojson::value(static_cast<double>(max_val)));
    settings.emplace_back(picojson::value(s));
  };

  auto add_enum = [&](const char* sys, const char* sec, const char* key, const char* default_val,
                       const std::vector<std::pair<std::string, std::string>>& options) {
    picojson::object s;
    s.emplace("system", picojson::value(std::string(sys)));
    s.emplace("section", picojson::value(std::string(sec)));
    s.emplace("key", picojson::value(std::string(key)));
    s.emplace("type", picojson::value(std::string("enum")));
    s.emplace("value", picojson::value(GetConfigValue(sys, sec, key)));
    s.emplace("default", picojson::value(std::string(default_val)));
    picojson::array opts;
    for (const auto& [val, label] : options)
    {
      picojson::object opt;
      opt.emplace("value", picojson::value(val));
      opt.emplace("label", picojson::value(label));
      opts.emplace_back(picojson::value(opt));
    }
    s.emplace("options", picojson::value(opts));
    settings.emplace_back(picojson::value(s));
  };

  if (section == "controllers")
  {
    for (int i = 0; i < 4; ++i)
    {
      add_enum("Dolphin", "Core", fmt::format("SIDevice{}", i).c_str(), "6",
               {{"0", "None"},
                {"6", "Standard Controller"},
                {"7", "Steering Wheel"},
                {"8", "Dance Mat"},
                {"11", "GBA"},
                {"12", "GC Adapter"},
                {"13", "Keyboard"}});
    }
    for (int i = 1; i <= 4; ++i)
    {
      add_enum("Wiimote", fmt::format("Wiimote{}", i).c_str(), "Source", "1",
               {{"0", "None"}, {"1", "Emulated"}, {"2", "Real"}});
    }
    add_bool("Dolphin", "BluetoothPassthrough", "Enabled", false);
  }
  else if (section == "graphics")
  {
    add_enum("Dolphin", "Core", "GFXBackend", "",
             {{"D3D", "Direct3D 11"},
              {"D3D12", "Direct3D 12"},
              {"Vulkan", "Vulkan"},
              {"OGL", "OpenGL"},
              {"Software Renderer", "Software Renderer"},
              {"Null", "Null"}});
    add_int("Graphics", "Settings", "InternalResolution", 1, 1, 8);
    add_enum("Graphics", "Settings", "AspectRatio", "0",
             {{"0", "Auto"}, {"1", "Force 16:9"}, {"2", "Force 4:3"}, {"3", "Stretch to Window"}});
    add_bool("Graphics", "Settings", "VSync", false);
    add_int("Graphics", "Settings", "MSAAX", 1, 1, 8);
    add_bool("Graphics", "Settings", "ShowFPS", false);
    add_enum("Graphics", "Enhancements", "ForceTextureFiltering", "0",
             {{"0", "Default"}, {"1", "Near"}, {"2", "Linear"}});
  }
  else if (section == "audio")
  {
    add_int("Dolphin", "DSP", "Volume", 100, 0, 100);
    add_enum("Dolphin", "DSP", "Backend", "Cubeb",
             {{"Cubeb", "Cubeb"},
              {"OpenAL", "OpenAL"},
              {"PulseAudio", "PulseAudio"},
              {"No Audio Output", "No Audio Output"}});
    add_bool("Dolphin", "DSP", "EnableJIT", true);
    add_bool("Dolphin", "DSP", "AudioStretch", false);
    add_bool("Dolphin", "Core", "AudioMuted", false);
  }
  else if (section == "paths")
  {
    // ISO paths are dynamic; report current list.
    const auto paths = Config::GetIsoPaths();
    picojson::object s;
    s.emplace("system", picojson::value(std::string("Dolphin")));
    s.emplace("section", picojson::value(std::string("General")));
    s.emplace("key", picojson::value(std::string("ISOPaths")));
    s.emplace("type", picojson::value(std::string("path_list")));
    picojson::array path_arr;
    for (const auto& p : paths)
      path_arr.emplace_back(picojson::value(p));
    s.emplace("value", picojson::value(path_arr));
    settings.emplace_back(picojson::value(s));
  }
  else if (section == "core")
  {
    add_enum("Dolphin", "Core", "CPUCore", "1",
             {{"0", "Interpreter"}, {"1", "JIT Recompiler"}, {"5", "JITIL Recompiler"}});
    add_bool("Dolphin", "Core", "CPUThread", true);
    add_float("Dolphin", "Core", "EmulationSpeed", 1.0f, 0.0f, 10.0f);
    add_bool("Dolphin", "Core", "OverclockEnable", false);
    add_float("Dolphin", "Core", "Overclock", 1.0f, 0.1f, 10.0f);
    add_bool("Dolphin", "Core", "MMU", false);
    add_bool("Dolphin", "Core", "DCBZ", false);
    add_bool("Dolphin", "Core", "FPRF", false);
    add_bool("Dolphin", "Core", "SyncGPU", false);
  }
  else
  {
    return "ERR Unknown section. Valid: controllers, graphics, audio, paths, core";
  }

  picojson::object result;
  result.emplace("ok", picojson::value(true));
  result.emplace("section", picojson::value(section));
  result.emplace("settings", picojson::value(settings));
  return picojson::value(result).serialize();
}

// ---------------------------------------------------------------------------
// Pause / Resume
// ---------------------------------------------------------------------------

static std::string HandlePause(Core::System& system)
{
  if (!Core::IsRunning(system))
    return "ERR Emulation not running";
  Core::SetState(system, Core::State::Paused);
  return "OK";
}

static std::string HandleResume(Core::System& system)
{
  if (Core::GetState(system) != Core::State::Paused)
    return "ERR Emulation not paused";
  Core::SetState(system, Core::State::Running);
  return "OK";
}

// ---------------------------------------------------------------------------
// TAS Movie commands
// Uses Movie::MovieManager — see Core/Movie.h for the source enum/API.
// ---------------------------------------------------------------------------

static std::string HandlePlayMovie(Core::System& system, const std::string& path)
{
  auto& movie = system.GetMovie();
  if (movie.IsMovieActive())
    return "ERR Movie already active (stop it first)";

  movie.SetReadOnly(true);
  std::optional<std::string> savestate_path;
  if (!movie.PlayInput(path, &savestate_path))
    return "ERR Failed to load movie file";

  return "OK";
}

static std::string HandleRecordMovie(Core::System& system)
{
  auto& movie = system.GetMovie();
  if (movie.IsMovieActive())
    return "ERR Movie already active (stop it first)";

  // Build controller arrays from current config — mirrors MainWindow::OnStartRecording().
  // Source: Movie::ControllerType enum in Core/Movie.h,
  //         Config::GetInfoForSIDevice in Core/Config/MainSettings.h,
  //         Config::GetInfoForWiimoteSource in Core/Config/WiimoteSettings.h
  Movie::ControllerTypeArray controllers{};
  Movie::WiimoteEnabledArray wiimotes{};
  for (int i = 0; i < 4; i++)
  {
    const auto si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      controllers[i] = Movie::ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(si_device))
      controllers[i] = Movie::ControllerType::GC;
    else
      controllers[i] = Movie::ControllerType::None;
    wiimotes[i] = Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::None;
  }

  movie.SetReadOnly(false);
  if (!movie.BeginRecordingInput(controllers, wiimotes))
    return "ERR Failed to start recording";

  return "OK";
}

static std::string HandleStopMovie(Core::System& system)
{
  auto& movie = system.GetMovie();
  if (!movie.IsMovieActive())
    return "ERR No movie active";

  movie.EndPlayInput(false);
  return "OK";
}

static std::string HandleSaveMovie(Core::System& system, const std::string& path)
{
  auto& movie = system.GetMovie();
  if (!movie.IsRecordingInput() && !movie.IsPlayingInput())
    return "ERR No movie active to save";

  movie.SaveRecording(path);
  return "OK";
}

// ---------------------------------------------------------------------------
// CreateHandlers — wire shared logic + frontend callbacks into a CommandHandler
// ---------------------------------------------------------------------------

CommandHandler CreateHandlers(Core::System& system, FrontendCallbacks frontend)
{
  CommandHandler handler;

  // --- Frontend-specific: boot, boot_nand, stop, fullscreen ---

  handler.on_boot = [&system, boot_fn = std::move(frontend.boot_game),
                     boot_path_ptr = handler.last_boot_path](
                        const std::string& b64path) -> std::string {
    const std::string path = DecodeBase64(b64path);
    if (path.empty())
      return "ERR Invalid base64 path";
    *boot_path_ptr = path;
    boot_fn(path);
    return "OK";
  };

  handler.on_boot_nand = [boot_nand_fn = std::move(frontend.boot_nand),
                          boot_path_ptr = handler.last_boot_path](
                             u64 title_id) -> std::string {
    boot_path_ptr->clear();
    boot_nand_fn(title_id);
    return "OK";
  };

  handler.on_stop = [stop_fn = std::move(frontend.force_stop),
                     boot_path_ptr = handler.last_boot_path]() -> std::string {
    boot_path_ptr->clear();
    stop_fn();
    return "OK";
  };

  handler.on_exit = [exit_fn = std::move(frontend.exit_app),
                     stop_fn = handler.on_stop]() -> std::string {
    if (stop_fn)
      stop_fn();
    if (exit_fn)
      exit_fn();
    return "OK";
  };

  handler.on_fullscreen_toggle = std::move(frontend.fullscreen_toggle);

  handler.on_get_fullscreen = [is_fs = frontend.is_fullscreen]() -> std::string {
    if (!is_fs)
      return "ERR Not supported";
    return is_fs() ? "OK true" : "OK false";
  };

  // --- Shared handlers ---

  handler.on_status = [&system]() -> std::string { return GetCoreStateString(system); };

  handler.on_install_wad = [&system](const std::string& b64path) {
    return HandleInstallWAD(system, b64path);
  };
  handler.on_uninstall_title = [&system](u64 tid) {
    return HandleUninstallTitle(system, tid);
  };
  handler.on_is_title_installed = [&system](u64 tid) {
    return HandleIsTitleInstalled(system, tid);
  };
  handler.on_list_titles = [&system]() { return HandleListTitles(system); };

  handler.on_get_config = [](const std::string& sys, const std::string& sec,
                             const std::string& key) { return HandleGetConfig(sys, sec, key); };
  handler.on_set_config = [](const std::string& sys, const std::string& sec,
                             const std::string& key,
                             const std::string& val) { return HandleSetConfig(sys, sec, key, val); };

  handler.on_wiimote_sync = [&system]() { return HandleWiimoteSync(system); };
  handler.on_wiimote_refresh = []() { return HandleWiimoteRefresh(); };
  handler.on_gc_change_device = [&system](int ch, int type) {
    return HandleGCChangeDevice(system, ch, type);
  };
  handler.on_gc_adapter_status = []() { return HandleGCAdapterStatus(); };

  handler.on_scan_game = [](const std::string& b64path) { return HandleScanGame(b64path); };
  handler.on_get_banner = [](const std::string& b64path) { return HandleGetBanner(b64path); };

  // --- v1 expansion: save states, screenshot, system info, volume, config, speed, gecko ---

  handler.on_save_state = [&system](int slot) { return HandleSaveState(system, slot); };
  handler.on_load_state = [&system](int slot) { return HandleLoadState(system, slot); };
  handler.on_list_save_states = []() { return HandleListSaveStates(); };
  handler.on_save_state_file = [&system](const std::string& b64path) {
    return HandleSaveStateFile(system, b64path);
  };
  handler.on_load_state_file = [&system](const std::string& b64path) {
    return HandleLoadStateFile(system, b64path);
  };
  handler.on_screenshot = [](const std::string& name) { return HandleScreenshot(name); };
  handler.on_get_system_info = []() { return HandleGetSystemInfo(); };
  handler.on_set_volume = [&system](int vol) { return HandleSetVolume(system, vol); };
  handler.on_get_volume = []() { return HandleGetVolume(); };
  handler.on_toggle_mute = [&system]() { return HandleToggleMute(system); };
  handler.on_dump_config = [](const std::string& filter) { return HandleDumpConfig(filter); };
  handler.on_get_config_schema = [](const std::string& section) {
    return HandleGetConfigSchema(section);
  };
  handler.on_get_gecko_port = []() { return HandleGetGeckoPort(); };
  handler.on_set_speed = [](float speed) { return HandleSetSpeed(speed); };
  handler.on_get_speed = []() { return HandleGetSpeed(); };
  handler.on_frame_step = [&system]() { return HandleFrameStep(system); };
  handler.on_get_game_paths = []() { return HandleGetGamePaths(); };

  // Frontend-dispatched: disc ops, list_games
  handler.on_change_disc = std::move(frontend.change_disc);
  handler.on_eject_disc = std::move(frontend.eject_disc);
  handler.on_list_games = std::move(frontend.list_games);

  // Pause / resume
  handler.on_pause = [&system]() { return HandlePause(system); };
  handler.on_resume = [&system]() { return HandleResume(system); };

  // TAS movie
  handler.on_play_movie = [&system](const std::string& path) {
    return HandlePlayMovie(system, path);
  };
  handler.on_record_movie = [&system]() { return HandleRecordMovie(system); };
  handler.on_stop_movie = [&system]() { return HandleStopMovie(system); };
  handler.on_save_movie = [&system](const std::string& path) {
    return HandleSaveMovie(system, path);
  };

  return handler;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static picojson::value JsonOk()
{
  picojson::object obj;
  obj.emplace("ok", picojson::value(true));
  return picojson::value(obj);
}

static picojson::value JsonOkWith(const std::string& key, const picojson::value& val)
{
  picojson::object obj;
  obj.emplace("ok", picojson::value(true));
  obj.emplace(key, val);
  return picojson::value(obj);
}

static picojson::value JsonError(const std::string& message)
{
  picojson::object obj;
  obj.emplace("error", picojson::value(message));
  return picojson::value(obj);
}

// Read a required string field from a JSON object.
static std::optional<std::string> JsonGetString(const picojson::object& obj, const std::string& key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>())
    return {};
  return it->second.get<std::string>();
}

// Read an optional integer field (JSON numbers are doubles).
static std::optional<int> JsonGetInt(const picojson::object& obj, const std::string& key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<double>())
    return {};
  return static_cast<int>(it->second.get<double>());
}

// Read an optional double field.
static std::optional<double> JsonGetDouble(const picojson::object& obj, const std::string& key)
{
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<double>())
    return {};
  return it->second.get<double>();
}

// ---------------------------------------------------------------------------
// HandleJsonCommand — dispatches JSON requests to the same handler logic
// ---------------------------------------------------------------------------

static std::string HandleJsonCommand(const std::string& line, const CommandHandler& handler)
{
  picojson::value parsed;
  const std::string err = picojson::parse(parsed, line);
  if (!err.empty() || !parsed.is<picojson::object>())
    return JsonError("Invalid JSON").serialize();

  const auto& obj = parsed.get<picojson::object>();
  const auto cmd = JsonGetString(obj, "cmd");
  if (!cmd)
    return JsonError("Missing \"cmd\" field").serialize();

  const std::string& c = *cmd;

  // --- ping / version / status ---
  if (c == "ping")
  {
    return JsonOk().serialize();
  }
  else if (c == "version")
  {
    return JsonOkWith("version", picojson::value(static_cast<double>(PROTOCOL_VERSION))).serialize();
  }
  else if (c == "status")
  {
    if (!handler.on_status)
      return JsonError("Not implemented").serialize();
    // on_status now returns a Core::State string via CoreStateToString()
    const std::string state = handler.on_status();

    picojson::object resp;
    resp.emplace("ok", picojson::value(true));
    resp.emplace("state", picojson::value(state));

    if (state == "running")
    {
      const auto& sconfig = SConfig::GetInstance();
      const auto& sys = Core::System::GetInstance();

      resp.emplace("game_id", picojson::value(sconfig.GetGameID()));
      resp.emplace("title", picojson::value(sconfig.GetTitleName()));

      const std::string platform = sys.IsTriforce() ? "triforce" :
                                   sys.IsWii()      ? "wii" :
                                                       "gamecube";
      resp.emplace("platform", picojson::value(platform));

      // Path is only meaningful for disc/executable boots, not NAND titles.
      const bool has_path = !handler.last_boot_path->empty();
      resp.emplace("has_path", picojson::value(has_path));

      // The boot path is tracked by the handler when BOOT is called.
      // For NAND titles, path is empty.
      resp.emplace("path", picojson::value(*handler.last_boot_path));
    }

    return picojson::value(resp).serialize();
  }
  // --- boot ---
  else if (c == "boot")
  {
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing \"path\" field").serialize();
    if (!handler.on_boot)
      return JsonError("Not implemented").serialize();
    // Encode path to base64 for the handler (which expects b64).
    std::string b64 = EncodeBase64(reinterpret_cast<const u8*>(path->data()), path->size());
    const std::string result = handler.on_boot(b64);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- boot_nand ---
  else if (c == "boot_nand")
  {
    const auto title_id_str = JsonGetString(obj, "title_id");
    if (!title_id_str)
      return JsonError("Missing \"title_id\" field").serialize();
    const auto title_id = ParseHexU64(*title_id_str);
    if (!title_id)
      return JsonError("Invalid title ID (expected hex string)").serialize();
    if (!handler.on_boot_nand)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_boot_nand(*title_id);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- stop ---
  else if (c == "stop")
  {
    if (!handler.on_stop)
      return JsonError("Not implemented").serialize();
    handler.on_stop();
    return JsonOk().serialize();
  }
  // --- exit ---
  else if (c == "exit")
  {
    if (!handler.on_exit)
      return JsonError("Not implemented").serialize();
    handler.on_exit();
    return JsonOk().serialize();
  }
  // --- get_config ---
  else if (c == "get_config")
  {
    const auto sys = JsonGetString(obj, "system");
    const auto sec = JsonGetString(obj, "section");
    const auto key = JsonGetString(obj, "key");
    if (!sys || !sec || !key)
      return JsonError("Missing system/section/key fields").serialize();
    if (!handler.on_get_config)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_get_config(*sys, *sec, *key);
    if (result.substr(0, 3) == "OK ")
      return JsonOkWith("value", picojson::value(result.substr(3))).serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- set_config (single or batch) ---
  else if (c == "set_config")
  {
    if (!handler.on_set_config)
      return JsonError("Not implemented").serialize();

    // Check for batch mode: "settings" array.
    auto settings_it = obj.find("settings");
    if (settings_it != obj.end() && settings_it->second.is<picojson::array>())
    {
      const auto& settings = settings_it->second.get<picojson::array>();
      if (settings.empty())
        return JsonError("Empty settings array").serialize();

      // Apply all settings to the Base layer without saving/notifying between each one.
      auto base_layer = Config::GetLayer(Config::LayerType::Base);
      picojson::array errors;

      for (const auto& entry : settings)
      {
        if (!entry.is<picojson::object>())
        {
          errors.emplace_back(picojson::value("Non-object entry in settings array"));
          continue;
        }
        const auto& s = entry.get<picojson::object>();
        const auto sys = JsonGetString(s, "system");
        const auto sec = JsonGetString(s, "section");
        const auto key = JsonGetString(s, "key");
        const auto val = JsonGetString(s, "value");
        if (!sys || !sec || !key || !val)
        {
          errors.emplace_back(picojson::value("Missing system/section/key/value in entry"));
          continue;
        }
        auto system = ResolveConfigSystem(*sys);
        if (!system)
        {
          errors.emplace_back(
              picojson::value(fmt::format("Invalid system name: {}", *sys)));
          continue;
        }
        base_layer->Set(Config::Location{*system, *sec, *key}, *val);
      }

      // Single notify + save for the entire batch.
      Config::OnConfigChanged();
      Config::Save();

      if (!errors.empty())
      {
        picojson::object result;
        result.emplace("ok", picojson::value(true));
        result.emplace("errors", picojson::value(errors));
        return picojson::value(result).serialize();
      }
      return JsonOk().serialize();
    }

    // Single set_config (non-batch).
    const auto sys = JsonGetString(obj, "system");
    const auto sec = JsonGetString(obj, "section");
    const auto key = JsonGetString(obj, "key");
    const auto val = JsonGetString(obj, "value");
    if (!sys || !sec || !key || !val)
      return JsonError("Missing system/section/key/value fields").serialize();
    const std::string result = handler.on_set_config(*sys, *sec, *key, *val);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- install_wad ---
  else if (c == "install_wad")
  {
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing \"path\" field").serialize();
    if (!handler.on_install_wad)
      return JsonError("Not implemented").serialize();
    std::string b64 = EncodeBase64(reinterpret_cast<const u8*>(path->data()), path->size());
    const std::string result = handler.on_install_wad(b64);
    if (result.substr(0, 2) == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- uninstall_title ---
  else if (c == "uninstall_title")
  {
    const auto tid_str = JsonGetString(obj, "title_id");
    if (!tid_str)
      return JsonError("Missing \"title_id\" field").serialize();
    const auto title_id = ParseHexU64(*tid_str);
    if (!title_id)
      return JsonError("Invalid title ID").serialize();
    if (!handler.on_uninstall_title)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_uninstall_title(*title_id);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- is_title_installed ---
  else if (c == "is_title_installed")
  {
    const auto tid_str = JsonGetString(obj, "title_id");
    if (!tid_str)
      return JsonError("Missing \"title_id\" field").serialize();
    const auto title_id = ParseHexU64(*tid_str);
    if (!title_id)
      return JsonError("Invalid title ID").serialize();
    if (!handler.on_is_title_installed)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_is_title_installed(*title_id);
    return JsonOkWith("installed", picojson::value(result == "OK TRUE")).serialize();
  }
  // --- list_titles ---
  else if (c == "list_titles")
  {
    if (!handler.on_list_titles)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_list_titles();
    if (result.substr(0, 2) != "OK")
      return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
    // Parse "OK <tid1> <tid2> ..." into an array.
    picojson::array titles;
    auto parts = SplitArgs(result, 999);
    for (size_t i = 1; i < parts.size(); ++i)
      titles.emplace_back(picojson::value(parts[i]));
    return JsonOkWith("titles", picojson::value(titles)).serialize();
  }
  // --- fullscreen_toggle ---
  else if (c == "fullscreen_toggle")
  {
    if (!handler.on_fullscreen_toggle)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_fullscreen_toggle();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- get_fullscreen ---
  else if (c == "get_fullscreen")
  {
    if (!handler.on_get_fullscreen)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_get_fullscreen();
    if (result.find("OK") == 0)
    {
      bool fs = result.find("true") != std::string::npos;
      return JsonOkWith("fullscreen", picojson::value(fs)).serialize();
    }
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- wiimote_sync ---
  else if (c == "wiimote_sync")
  {
    if (!handler.on_wiimote_sync)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_wiimote_sync();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- wiimote_refresh ---
  else if (c == "wiimote_refresh")
  {
    if (!handler.on_wiimote_refresh)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_wiimote_refresh();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- gc_change_device ---
  else if (c == "gc_change_device")
  {
    const auto channel = JsonGetInt(obj, "channel");
    const auto device_type = JsonGetInt(obj, "device_type");
    if (!channel || !device_type)
      return JsonError("Missing channel/device_type fields").serialize();
    if (!handler.on_gc_change_device)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_gc_change_device(*channel, *device_type);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- gc_adapter_status ---
  else if (c == "gc_adapter_status")
  {
    if (!handler.on_gc_adapter_status)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_gc_adapter_status();
    picojson::object out;
    out.emplace("ok", picojson::value(true));
    out.emplace("detected", picojson::value(result == "OK DETECTED"));
    if (result.find("NOT_DETECTED ") != std::string::npos)
      out.emplace("message", picojson::value(result.substr(result.find("NOT_DETECTED ") + 13)));
    return picojson::value(out).serialize();
  }
  // --- scan_game ---
  else if (c == "scan_game")
  {
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing \"path\" field").serialize();
    if (!handler.on_scan_game)
      return JsonError("Not implemented").serialize();
    std::string b64 = EncodeBase64(reinterpret_cast<const u8*>(path->data()), path->size());
    const std::string result = handler.on_scan_game(b64);
    if (result.substr(0, 3) != "OK ")
      return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
    // Parse "OK GAME_ID:xxx TITLE:yyy ..." into JSON object.
    picojson::object game;
    game.emplace("ok", picojson::value(true));
    // Use regex-like parsing to extract KEY:VALUE pairs.
    const std::string payload = result.substr(3);
    // Match keys: GAME_ID, TITLE, REGION, PLATFORM, DISC_NUMBER, REVISION
    const std::vector<std::string> keys = {"GAME_ID",    "TITLE",    "REGION",  "PLATFORM",
                                           "DISC_NUMBER", "REVISION", "MAKER_ID", "MAKER"};
    for (size_t i = 0; i < keys.size(); ++i)
    {
      const std::string prefix = keys[i] + ":";
      size_t start = payload.find(prefix);
      if (start == std::string::npos)
        continue;
      start += prefix.size();
      // Value runs until the next KEY: or end of string.
      size_t end = std::string::npos;
      for (size_t j = i + 1; j < keys.size(); ++j)
      {
        size_t next = payload.find(" " + keys[j] + ":", start);
        if (next != std::string::npos)
        {
          end = next;
          break;
        }
      }
      std::string value = (end != std::string::npos) ? payload.substr(start, end - start)
                                                     : payload.substr(start);
      // Trim trailing whitespace.
      while (!value.empty() && value.back() == ' ')
        value.pop_back();
      // Convert snake_case key name for JSON.
      std::string json_key = keys[i];
      std::transform(json_key.begin(), json_key.end(), json_key.begin(), ::tolower);
      // disc_number and revision as numbers.
      if (keys[i] == "DISC_NUMBER" || keys[i] == "REVISION")
      {
        const auto num = ParseInt(value);
        game.emplace(json_key, picojson::value(static_cast<double>(num.value_or(0))));
      }
      else
      {
        game.emplace(json_key, picojson::value(value));
      }
    }
    return picojson::value(game).serialize();
  }
  // --- get_banner ---
  else if (c == "get_banner")
  {
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing \"path\" field").serialize();
    if (!handler.on_get_banner)
      return JsonError("Not implemented").serialize();
    std::string b64 = EncodeBase64(reinterpret_cast<const u8*>(path->data()), path->size());
    const std::string result = handler.on_get_banner(b64);
    if (result.substr(0, 3) == "OK ")
      return JsonOkWith("data", picojson::value(result.substr(3))).serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- v1 expansion: save states ---
  else if (c == "save_state")
  {
    const auto slot = JsonGetInt(obj, "slot");
    if (!slot)
      return JsonError("Missing \"slot\" field (1-10)").serialize();
    if (!handler.on_save_state)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_save_state(*slot);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "load_state")
  {
    const auto slot = JsonGetInt(obj, "slot");
    if (!slot)
      return JsonError("Missing \"slot\" field (1-10)").serialize();
    if (!handler.on_load_state)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_load_state(*slot);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "list_save_states")
  {
    if (!handler.on_list_save_states)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_list_save_states();
    // Parse multiline "OK\nSLOT 1 ...\nSLOT 2 ..." into JSON array.
    picojson::array slots;
    std::istringstream stream(result);
    std::string line_buf;
    std::getline(stream, line_buf);  // skip "OK"
    while (std::getline(stream, line_buf))
    {
      // Each line: "SLOT N info" or "SLOT N EMPTY"
      picojson::object slot_obj;
      if (line_buf.size() > 5 && line_buf.substr(0, 5) == "SLOT ")
      {
        size_t sp = line_buf.find(' ', 5);
        if (sp != std::string::npos)
        {
          const auto slot_num = ParseInt(line_buf.data() + 5, line_buf.data() + sp);
          slot_obj.emplace("slot", picojson::value(static_cast<double>(slot_num.value_or(0))));
          std::string info = line_buf.substr(sp + 1);
          slot_obj.emplace("empty", picojson::value(info == "EMPTY"));
          if (info != "EMPTY")
            slot_obj.emplace("info", picojson::value(info));
        }
      }
      if (!slot_obj.empty())
        slots.emplace_back(picojson::value(slot_obj));
    }
    return JsonOkWith("slots", picojson::value(slots)).serialize();
  }
  else if (c == "save_state_file")
  {
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing \"path\" field").serialize();
    if (!handler.on_save_state_file)
      return JsonError("Not implemented").serialize();
    std::string b64 = EncodeBase64(reinterpret_cast<const u8*>(path->data()), path->size());
    const std::string result = handler.on_save_state_file(b64);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "load_state_file")
  {
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing \"path\" field").serialize();
    if (!handler.on_load_state_file)
      return JsonError("Not implemented").serialize();
    std::string b64 = EncodeBase64(reinterpret_cast<const u8*>(path->data()), path->size());
    const std::string result = handler.on_load_state_file(b64);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- screenshot ---
  else if (c == "screenshot")
  {
    if (!handler.on_screenshot)
      return JsonError("Not implemented").serialize();
    const auto name = JsonGetString(obj, "name");
    const std::string result = handler.on_screenshot(name.value_or(""));
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- system info ---
  else if (c == "get_system_info")
  {
    if (!handler.on_get_system_info)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_get_system_info();
    // Parse "OK VERSION:x BRANCH:y ..." into structured JSON.
    picojson::object info;
    info.emplace("ok", picojson::value(true));
    const std::string payload = result.substr(3);  // skip "OK "
    const std::vector<std::string> keys = {"VERSION",  "BRANCH",      "REVISION", "OS",
                                           "IPC_VERSION", "BACKEND", "USER_DIR"};
    for (size_t i = 0; i < keys.size(); ++i)
    {
      const std::string prefix = keys[i] + ":";
      size_t start = payload.find(prefix);
      if (start == std::string::npos)
        continue;
      start += prefix.size();
      size_t end = std::string::npos;
      for (size_t j = i + 1; j < keys.size(); ++j)
      {
        size_t next = payload.find(" " + keys[j] + ":", start);
        if (next != std::string::npos)
        {
          end = next;
          break;
        }
      }
      std::string value = (end != std::string::npos) ? payload.substr(start, end - start)
                                                     : payload.substr(start);
      while (!value.empty() && value.back() == ' ')
        value.pop_back();
      std::string json_key = keys[i];
      std::transform(json_key.begin(), json_key.end(), json_key.begin(), ::tolower);
      if (keys[i] == "IPC_VERSION")
      {
        const auto num = ParseInt(value);
        if (num)
          info.emplace(json_key, picojson::value(static_cast<double>(*num)));
        else
          info.emplace(json_key, picojson::value(value));
      }
      else
      {
        info.emplace(json_key, picojson::value(value));
      }
    }
    return picojson::value(info).serialize();
  }
  // --- volume ---
  else if (c == "set_volume")
  {
    const auto vol = JsonGetInt(obj, "volume");
    if (!vol)
      return JsonError("Missing \"volume\" field (0-100)").serialize();
    if (!handler.on_set_volume)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_set_volume(*vol);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "get_volume")
  {
    if (!handler.on_get_volume)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_get_volume();
    if (result.substr(0, 3) == "OK ")
    {
      const auto vol = ParseInt(result.substr(3));
      if (vol)
        return JsonOkWith("volume", picojson::value(static_cast<double>(*vol))).serialize();
    }
    return JsonError("Failed to get volume").serialize();
  }
  else if (c == "toggle_mute")
  {
    if (!handler.on_toggle_mute)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_toggle_mute();
    if (result.substr(0, 3) == "OK ")
    {
      bool muted = result.find("MUTED") != std::string::npos &&
                   result.find("UNMUTED") == std::string::npos;
      return JsonOkWith("muted", picojson::value(muted)).serialize();
    }
    return JsonError("Failed to toggle mute").serialize();
  }
  // --- config dump ---
  else if (c == "dump_config")
  {
    if (!handler.on_dump_config)
      return JsonError("Not implemented").serialize();
    const auto sys_filter = JsonGetString(obj, "system");
    const std::string result = handler.on_dump_config(sys_filter.value_or(""));
    if (result.substr(0, 2) != "OK")
      return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
    // Parse multiline "OK\nSys.Sec.Key=Val" into JSON object.
    picojson::object config;
    std::istringstream stream(result);
    std::string line_buf;
    std::getline(stream, line_buf);  // skip "OK"
    while (std::getline(stream, line_buf))
    {
      size_t eq = line_buf.find('=');
      if (eq != std::string::npos)
        config.emplace(line_buf.substr(0, eq), picojson::value(line_buf.substr(eq + 1)));
    }
    return JsonOkWith("config", picojson::value(config)).serialize();
  }
  // --- config schema ---
  else if (c == "get_config_schema")
  {
    const auto section = JsonGetString(obj, "section");
    if (!section)
      return JsonError("Missing \"section\" field").serialize();
    if (!handler.on_get_config_schema)
      return JsonError("Not implemented").serialize();
    // get_config_schema already returns JSON, just pass through.
    const std::string result = handler.on_get_config_schema(*section);
    if (result.substr(0, 4) == "ERR ")
      return JsonError(result.substr(4)).serialize();
    return result;  // Already JSON.
  }
  // --- disc operations ---
  else if (c == "change_disc")
  {
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing \"path\" field").serialize();
    if (!handler.on_change_disc)
      return JsonError("Not implemented").serialize();
    // Encode to base64 for the handler.
    std::string b64 = EncodeBase64(reinterpret_cast<const u8*>(path->data()), path->size());
    const std::string result = handler.on_change_disc(b64);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "eject_disc")
  {
    if (!handler.on_eject_disc)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_eject_disc();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- gecko port ---
  else if (c == "get_gecko_port")
  {
    if (!handler.on_get_gecko_port)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_get_gecko_port();
    if (result == "OK INACTIVE")
    {
      picojson::object out;
      out.emplace("ok", picojson::value(true));
      out.emplace("active", picojson::value(false));
      return picojson::value(out).serialize();
    }
    if (result.substr(0, 3) == "OK ")
    {
      picojson::object out;
      out.emplace("ok", picojson::value(true));
      out.emplace("active", picojson::value(true));
      const auto port_num = ParseInt(result.substr(3));
      if (port_num)
        out.emplace("port", picojson::value(static_cast<double>(*port_num)));
      else
        out.emplace("port", picojson::value(result.substr(3)));
      return picojson::value(out).serialize();
    }
    return JsonError("Failed to get gecko port").serialize();
  }
  // --- speed / frame ---
  else if (c == "set_speed")
  {
    const auto speed = JsonGetDouble(obj, "speed");
    if (!speed)
      return JsonError("Missing \"speed\" field").serialize();
    if (!handler.on_set_speed)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_set_speed(static_cast<float>(*speed));
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "get_speed")
  {
    if (!handler.on_get_speed)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_get_speed();
    if (result.substr(0, 3) == "OK ")
    {
      const auto spd = ParseDouble(result.substr(3));
      if (spd)
        return JsonOkWith("speed", picojson::value(*spd)).serialize();
    }
    return JsonError("Failed to get speed").serialize();
  }
  else if (c == "frame_step")
  {
    if (!handler.on_frame_step)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_frame_step();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- game library ---
  else if (c == "list_games")
  {
    if (!handler.on_list_games)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_list_games();
    if (result.substr(0, 4) == "ERR ")
      return JsonError(result.substr(4)).serialize();
    // list_games returns JSON directly from the frontend callback.
    if (!result.empty() && result[0] == '{')
      return result;
    // Otherwise wrap the text result.
    return JsonOkWith("data", picojson::value(result)).serialize();
  }
  else if (c == "get_game_paths")
  {
    if (!handler.on_get_game_paths)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_get_game_paths();
    if (result.substr(0, 2) != "OK")
      return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
    // Parse multiline "OK\npath1\npath2" into array.
    picojson::array paths;
    std::istringstream stream(result);
    std::string line_buf;
    std::getline(stream, line_buf);  // skip "OK"
    while (std::getline(stream, line_buf))
    {
      if (!line_buf.empty())
        paths.emplace_back(picojson::value(line_buf));
    }
    return JsonOkWith("paths", picojson::value(paths)).serialize();
  }
  // --- Pause / Resume ---
  else if (c == "pause")
  {
    if (!handler.on_pause)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_pause();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "resume")
  {
    if (!handler.on_resume)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_resume();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  // --- TAS Movie ---
  else if (c == "play_movie")
  {
    if (!handler.on_play_movie)
      return JsonError("Not implemented").serialize();
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing 'path' parameter").serialize();
    const std::string result = handler.on_play_movie(*path);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "record_movie")
  {
    if (!handler.on_record_movie)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_record_movie();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "stop_movie")
  {
    if (!handler.on_stop_movie)
      return JsonError("Not implemented").serialize();
    const std::string result = handler.on_stop_movie();
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else if (c == "save_movie")
  {
    if (!handler.on_save_movie)
      return JsonError("Not implemented").serialize();
    const auto path = JsonGetString(obj, "path");
    if (!path)
      return JsonError("Missing 'path' parameter").serialize();
    const std::string result = handler.on_save_movie(*path);
    if (result == "OK")
      return JsonOk().serialize();
    return JsonError(result.substr(0, 4) == "ERR " ? result.substr(4) : result).serialize();
  }
  else
  {
    return JsonError("Unknown command: " + c).serialize();
  }
}

// ---------------------------------------------------------------------------
// Server implementation
// ---------------------------------------------------------------------------

struct Server::ClientConnection
{
  std::unique_ptr<sf::TcpSocket> socket;
  std::thread thread;
  std::atomic<bool> running{true};
  std::mutex send_mutex;

  void Send(const std::string& message)
  {
    std::lock_guard lk(send_mutex);
    if (!running.load())
      return;
    std::string line = message + "\n";
    const char* ptr = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0)
    {
      std::size_t sent = 0;
      auto status = socket->send(ptr, remaining, sent);
      if (status == sf::Socket::Status::Disconnected ||
          status == sf::Socket::Status::Error)
      {
        running.store(false);
        return;
      }
      ptr += sent;
      remaining -= sent;
    }
  }
};

Server::Server(u16 port, CommandHandler handler) : m_port(port), m_handler(std::move(handler))
{
}

Server::~Server()
{
  Stop();
}

// Maps Core::State enum values to lowercase event names for IPC broadcast.
// Source enum: Core::State in Source/Core/Core/Core.h
static const char* CoreStateToString(Core::State state)
{
  switch (state)
  {
  case Core::State::Uninitialized:
    return "uninitialized";
  case Core::State::Paused:
    return "paused";
  case Core::State::Running:
    return "running";
  case Core::State::Stopping:
    return "stopping";
  case Core::State::Starting:
    return "starting";
  }
  return "unknown";
}

void Server::Start()
{
  if (m_running.load())
    return;

  m_running.store(true);
  m_server_thread = std::thread(&Server::ServerThread, this);

  // Broadcast state change events to all IPC clients.
  m_state_hook = Core::AddOnStateChangedCallback([this](Core::State state) {
    BroadcastEvent(fmt::format("EVENT {}", CoreStateToString(state)));
  });
}

void Server::Stop()
{
  if (!m_running.load())
    return;

  // Deregister before teardown so no callbacks fire during shutdown.
  m_state_hook.reset();

  m_running.store(false);

  if (m_server_thread.joinable())
    m_server_thread.join();

  // Disconnect all clients
  std::lock_guard lk(m_clients_mutex);
  for (auto& client : m_clients)
  {
    client->running.store(false);
    client->socket->disconnect();
    if (client->thread.joinable())
      client->thread.join();
  }
  m_clients.clear();
}

void Server::BroadcastEvent(const std::string& event)
{
  std::lock_guard lk(m_clients_mutex);
  for (auto& client : m_clients)
  {
    if (client->running.load())
      client->Send(event);
  }
}

void Server::ServerThread()
{
  Common::SetCurrentThreadName("DolphinIPC Server");

  sf::TcpListener listener;
  if (listener.listen(m_port) != sf::Socket::Status::Done)
  {
    ERROR_LOG_FMT(COMMON, "DolphinIPC: Failed to listen on port {}", m_port);
    m_running.store(false);
    return;
  }

  listener.setBlocking(false);
  INFO_LOG_FMT(COMMON, "DolphinIPC: Listening on TCP port {}", m_port);

  while (m_running.load())
  {
    // Accept new connections
    auto new_client = std::make_unique<sf::TcpSocket>();
    if (listener.accept(*new_client) == sf::Socket::Status::Done)
    {
      INFO_LOG_FMT(COMMON, "DolphinIPC: Client connected from {}:{}",
                   new_client->getRemoteAddress().value().toString(),
                   new_client->getRemotePort());

      auto connection = std::make_unique<ClientConnection>();
      connection->socket = std::move(new_client);
      connection->socket->setBlocking(false);

      auto* conn_ptr = connection.get();
      connection->thread = std::thread([this, conn_ptr]() {
        Common::SetCurrentThreadName("DolphinIPC Client");

        std::string buffer;

        while (conn_ptr->running.load() && m_running.load())
        {
          char recv_buf[1024];
          std::size_t received = 0;
          auto status = conn_ptr->socket->receive(recv_buf, sizeof(recv_buf), received);

          if (status == sf::Socket::Status::Disconnected ||
              status == sf::Socket::Status::Error)
          {
            INFO_LOG_FMT(COMMON, "DolphinIPC: Client disconnected");
            conn_ptr->running.store(false);
            break;
          }

          if (received > 0)
          {
            buffer.append(recv_buf, received);

            // Reject oversized buffers (prevents memory abuse).
            if (buffer.size() > MAX_COMMAND_LENGTH * 2)
            {
              WARN_LOG_FMT(COMMON, "DolphinIPC: Client buffer too large, disconnecting");
              conn_ptr->running.store(false);
              break;
            }

            // Process complete lines
            size_t newline_pos;
            while ((newline_pos = buffer.find('\n')) != std::string::npos)
            {
              std::string line = buffer.substr(0, newline_pos);
              buffer.erase(0, newline_pos + 1);

              // Strip \r if present (telnet sends \r\n)
              if (!line.empty() && line.back() == '\r')
                line.pop_back();

              if (line.empty())
                continue;

              // Reject oversized commands.
              if (line.size() > MAX_COMMAND_LENGTH)
              {
                conn_ptr->Send("ERR Command too long");
                continue;
              }

              std::string response = HandleCommand(line);
              if (!response.empty())
                conn_ptr->Send(response);
            }
          }
          else
          {
            Common::SleepCurrentThread(1);
          }
        }
      });

      std::lock_guard lk(m_clients_mutex);
      m_clients.push_back(std::move(connection));
    }

    // Clean up disconnected clients
    {
      std::lock_guard lk(m_clients_mutex);
      m_clients.erase(
          std::remove_if(m_clients.begin(), m_clients.end(),
                         [](const std::unique_ptr<ClientConnection>& c) {
                           if (!c->running.load())
                           {
                             if (c->thread.joinable())
                               c->thread.join();
                             return true;
                           }
                           return false;
                         }),
          m_clients.end());
    }

    Common::SleepCurrentThread(10);
  }
}

std::string Server::HandleCommand(const std::string& line)
{
  // Dual-mode: lines starting with '{' are JSON, everything else is text protocol.
  if (!line.empty() && line[0] == '{')
    return HandleJsonCommand(line, m_handler);

  // Parse command and arguments
  // Format: VERB [args...]
  std::string verb;
  std::string args;

  size_t space_pos = line.find(' ');
  if (space_pos != std::string::npos)
  {
    verb = line.substr(0, space_pos);
    args = line.substr(space_pos + 1);
  }
  else
  {
    verb = line;
  }

  // Normalize to uppercase
  std::transform(verb.begin(), verb.end(), verb.begin(), ::toupper);

  if (verb == "PING")
  {
    return "OK";
  }
  else if (verb == "VERSION")
  {
    return fmt::format("OK VERSION {}", PROTOCOL_VERSION);
  }
  else if (verb == "STATUS")
  {
    if (m_handler.on_status)
      return fmt::format("OK {}", m_handler.on_status());
    return "ERR Not implemented";
  }
  else if (verb == "BOOT")
  {
    if (args.empty())
      return "ERR Missing path argument";
    if (m_handler.on_boot)
      return m_handler.on_boot(args);
    return "ERR Not implemented";
  }
  else if (verb == "BOOT_NAND")
  {
    if (args.empty())
      return "ERR Missing title ID argument";
    const auto title_id = ParseHexU64(args);
    if (!title_id)
      return "ERR Invalid title ID (expected 16-char hex)";
    if (m_handler.on_boot_nand)
      return m_handler.on_boot_nand(*title_id);
    return "ERR Not implemented";
  }
  else if (verb == "STOP")
  {
    if (m_handler.on_stop)
      return m_handler.on_stop();
    return "ERR Not implemented";
  }
  else if (verb == "INSTALL_WAD")
  {
    if (args.empty())
      return "ERR Missing path argument";
    if (m_handler.on_install_wad)
      return m_handler.on_install_wad(args);
    return "ERR Not implemented";
  }
  else if (verb == "UNINSTALL_TITLE")
  {
    if (args.empty())
      return "ERR Missing title ID argument";
    const auto title_id = ParseHexU64(args);
    if (!title_id)
      return "ERR Invalid title ID (expected 16-char hex)";
    if (m_handler.on_uninstall_title)
      return m_handler.on_uninstall_title(*title_id);
    return "ERR Not implemented";
  }
  else if (verb == "IS_TITLE_INSTALLED")
  {
    if (args.empty())
      return "ERR Missing title ID argument";
    const auto title_id = ParseHexU64(args);
    if (!title_id)
      return "ERR Invalid title ID (expected 16-char hex)";
    if (m_handler.on_is_title_installed)
      return m_handler.on_is_title_installed(*title_id);
    return "ERR Not implemented";
  }
  else if (verb == "GET_CONFIG")
  {
    auto parts = SplitArgs(args, 3);
    if (parts.size() < 3)
      return "ERR Usage: GET_CONFIG <system> <section> <key>";
    if (m_handler.on_get_config)
      return m_handler.on_get_config(parts[0], parts[1], parts[2]);
    return "ERR Not implemented";
  }
  else if (verb == "SET_CONFIG")
  {
    auto parts = SplitArgs(args, 4);
    if (parts.size() < 4)
      return "ERR Usage: SET_CONFIG <system> <section> <key> <value>";
    if (m_handler.on_set_config)
      return m_handler.on_set_config(parts[0], parts[1], parts[2], parts[3]);
    return "ERR Not implemented";
  }
  else if (verb == "FULLSCREEN_TOGGLE")
  {
    if (m_handler.on_fullscreen_toggle)
      return m_handler.on_fullscreen_toggle();
    return "ERR Not implemented";
  }
  else if (verb == "WIIMOTE_SYNC")
  {
    if (m_handler.on_wiimote_sync)
      return m_handler.on_wiimote_sync();
    return "ERR Not implemented";
  }
  else if (verb == "WIIMOTE_REFRESH")
  {
    if (m_handler.on_wiimote_refresh)
      return m_handler.on_wiimote_refresh();
    return "ERR Not implemented";
  }
  else if (verb == "GC_CHANGE_DEVICE")
  {
    auto parts = SplitArgs(args, 2);
    if (parts.size() < 2)
      return "ERR Usage: GC_CHANGE_DEVICE <channel> <device_type>";
    const auto channel = ParseInt(parts[0]);
    const auto device_type = ParseInt(parts[1]);
    if (!channel || !device_type)
      return "ERR Invalid arguments (expected integers)";
    if (m_handler.on_gc_change_device)
      return m_handler.on_gc_change_device(*channel, *device_type);
    return "ERR Not implemented";
  }
  else if (verb == "GC_ADAPTER_STATUS")
  {
    if (m_handler.on_gc_adapter_status)
      return m_handler.on_gc_adapter_status();
    return "ERR Not implemented";
  }
  else if (verb == "SCAN_GAME")
  {
    if (args.empty())
      return "ERR Missing path argument";
    if (m_handler.on_scan_game)
      return m_handler.on_scan_game(args);
    return "ERR Not implemented";
  }
  else if (verb == "GET_BANNER")
  {
    if (args.empty())
      return "ERR Missing path argument";
    if (m_handler.on_get_banner)
      return m_handler.on_get_banner(args);
    return "ERR Not implemented";
  }
  else if (verb == "LIST_TITLES")
  {
    if (m_handler.on_list_titles)
      return m_handler.on_list_titles();
    return "ERR Not implemented";
  }
  // --- v1 expansion commands ---
  else if (verb == "SAVE_STATE")
  {
    if (args.empty())
      return "ERR Missing slot argument (1-10)";
    const auto slot = ParseInt(args);
    if (!slot)
      return "ERR Invalid slot (expected integer 1-10)";
    if (m_handler.on_save_state)
      return m_handler.on_save_state(*slot);
    return "ERR Not implemented";
  }
  else if (verb == "LOAD_STATE")
  {
    if (args.empty())
      return "ERR Missing slot argument (1-10)";
    const auto slot = ParseInt(args);
    if (!slot)
      return "ERR Invalid slot (expected integer 1-10)";
    if (m_handler.on_load_state)
      return m_handler.on_load_state(*slot);
    return "ERR Not implemented";
  }
  else if (verb == "LIST_SAVE_STATES")
  {
    if (m_handler.on_list_save_states)
      return m_handler.on_list_save_states();
    return "ERR Not implemented";
  }
  else if (verb == "SAVE_STATE_FILE")
  {
    if (args.empty())
      return "ERR Missing path argument (base64)";
    if (m_handler.on_save_state_file)
      return m_handler.on_save_state_file(args);
    return "ERR Not implemented";
  }
  else if (verb == "LOAD_STATE_FILE")
  {
    if (args.empty())
      return "ERR Missing path argument (base64)";
    if (m_handler.on_load_state_file)
      return m_handler.on_load_state_file(args);
    return "ERR Not implemented";
  }
  else if (verb == "SCREENSHOT")
  {
    if (m_handler.on_screenshot)
      return m_handler.on_screenshot(args);  // args may be empty (default name)
    return "ERR Not implemented";
  }
  else if (verb == "GET_SYSTEM_INFO")
  {
    if (m_handler.on_get_system_info)
      return m_handler.on_get_system_info();
    return "ERR Not implemented";
  }
  else if (verb == "SET_VOLUME")
  {
    if (args.empty())
      return "ERR Missing volume argument (0-100)";
    const auto volume = ParseInt(args);
    if (!volume)
      return "ERR Invalid volume (expected integer 0-100)";
    if (m_handler.on_set_volume)
      return m_handler.on_set_volume(*volume);
    return "ERR Not implemented";
  }
  else if (verb == "GET_VOLUME")
  {
    if (m_handler.on_get_volume)
      return m_handler.on_get_volume();
    return "ERR Not implemented";
  }
  else if (verb == "TOGGLE_MUTE")
  {
    if (m_handler.on_toggle_mute)
      return m_handler.on_toggle_mute();
    return "ERR Not implemented";
  }
  else if (verb == "DUMP_CONFIG")
  {
    if (m_handler.on_dump_config)
      return m_handler.on_dump_config(args);  // args = optional system filter
    return "ERR Not implemented";
  }
  else if (verb == "GET_CONFIG_SCHEMA")
  {
    if (args.empty())
      return "ERR Missing section argument (controllers, graphics, audio, paths, core)";
    if (m_handler.on_get_config_schema)
      return m_handler.on_get_config_schema(args);
    return "ERR Not implemented";
  }
  else if (verb == "CHANGE_DISC")
  {
    if (args.empty())
      return "ERR Missing path argument (base64)";
    if (m_handler.on_change_disc)
      return m_handler.on_change_disc(args);
    return "ERR Not implemented";
  }
  else if (verb == "EJECT_DISC")
  {
    if (m_handler.on_eject_disc)
      return m_handler.on_eject_disc();
    return "ERR Not implemented";
  }
  else if (verb == "GET_GECKO_PORT")
  {
    if (m_handler.on_get_gecko_port)
      return m_handler.on_get_gecko_port();
    return "ERR Not implemented";
  }
  else if (verb == "SET_SPEED")
  {
    if (args.empty())
      return "ERR Missing speed argument (float, 0=unlimited)";
    const auto speed = ParseFloat(args);
    if (!speed)
      return "ERR Invalid speed (expected float)";
    if (m_handler.on_set_speed)
      return m_handler.on_set_speed(*speed);
    return "ERR Not implemented";
  }
  else if (verb == "GET_SPEED")
  {
    if (m_handler.on_get_speed)
      return m_handler.on_get_speed();
    return "ERR Not implemented";
  }
  else if (verb == "FRAME_STEP")
  {
    if (m_handler.on_frame_step)
      return m_handler.on_frame_step();
    return "ERR Not implemented";
  }
  else if (verb == "LIST_GAMES")
  {
    if (m_handler.on_list_games)
      return m_handler.on_list_games();
    return "ERR Not implemented";
  }
  else if (verb == "GET_GAME_PATHS")
  {
    if (m_handler.on_get_game_paths)
      return m_handler.on_get_game_paths();
    return "ERR Not implemented";
  }
  else
  {
    return "ERR UNKNOWN_COMMAND";
  }
}

}  // namespace DolphinIPC
