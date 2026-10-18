#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <semaphore>

#include <CLI/CLI.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "model/project.h"
#include "runtime/plugin_loader.h"
#include "runtime/runtime.h"
#include "util/password.h"
#ifdef LOGICT_WITH_OPCUA
#include "opcua/opcua_server.h"
#endif
#ifdef LOGICT_WITH_MQTT
#include "mqtt/mqtt_agent.h"
#endif

namespace {

std::binary_semaphore g_shutdown{0};

void on_signal(int) { g_shutdown.release(); }

std::filesystem::path default_driver_dir(const char* argv0) {
  // Build tree: <build>/app/logictd/logictd -> <build>/drivers. Installed: <prefix>/lib/logict/drivers.
  auto exe = std::filesystem::weakly_canonical(std::filesystem::path(argv0));
  for (auto dir = exe.parent_path(); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path()) {
    if (std::filesystem::is_directory(dir / "drivers")) return dir / "drivers";
    if (std::filesystem::is_directory(dir / "lib/logict/drivers")) return dir / "lib/logict/drivers";
  }
  return "/usr/lib/logict/drivers";
}

std::optional<nlohmann::json> load_json(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    spdlog::critical("cannot open {}", path);
    return std::nullopt;
  }
  try {
    return nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
  } catch (const nlohmann::json::exception& e) {
    spdlog::critical("{} is not valid JSON: {}", path, e.what());
    return std::nullopt;
  }
}

int hash_password_command() {
  std::string password;
  if (isatty(STDIN_FILENO)) std::cerr << "Password: ";
  std::getline(std::cin, password);
  if (password.empty()) {
    std::cerr << "empty password\n";
    return 1;
  }
  std::cout << logict::core::hash_password(password) << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"logictd - industrial connectivity server"};
  std::string config_file;
  std::string project_file;
  std::string driver_dir;
  std::string log_level;
  std::uint32_t watch_ms = 0;
  bool hash_password = false;
  app.add_option("-c,--config", config_file, "Daemon configuration (JSON), e.g. /etc/logict/logictd.json");
  app.add_option("-p,--project", project_file, "Project JSON file (overrides the config)");
  app.add_option("-d,--drivers", driver_dir, "Driver plugin directory (overrides the config)");
  app.add_option("-l,--log-level", log_level, "trace|debug|info|warn|error");
  app.add_option("--watch", watch_ms, "Subscribe to every tag at this rate (ms) and log value changes");
  app.add_flag("--hash-password", hash_password, "Read a password from stdin and print its hash for the config");
  CLI11_PARSE(app, argc, argv);

  if (hash_password) return hash_password_command();

  spdlog::set_default_logger(spdlog::stdout_color_mt("logictd"));
  spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e %^%-5l%$ [%n] %v");

  nlohmann::json config = nlohmann::json::object();
  if (!config_file.empty()) {
    auto j = load_json(config_file);
    if (!j) return 1;
    config = std::move(*j);
  }
  if (log_level.empty()) log_level = logict::sdk::property<std::string>(config, "log_level", "info");
  if (project_file.empty())
    project_file = logict::sdk::property<std::string>(config, "project_file", "/var/lib/logict/project.json");
  if (driver_dir.empty()) driver_dir = logict::sdk::property<std::string>(config, "driver_dir", "");
  if (driver_dir.empty()) driver_dir = default_driver_dir(argv[0]).string();

  spdlog::set_level(spdlog::level::from_str(log_level));
  spdlog::info("{} {} starting", logict::core::kProductName, logict::core::kProductVersion);

  logict::core::DriverRegistry drivers;
  for (const auto& err : drivers.load_directory(driver_dir)) spdlog::error("{}", err);
  for (auto* d : drivers.drivers())
    spdlog::info("driver loaded: {} ({}) v{}", d->info().name, d->info().display_name, d->info().version);

  auto pj = load_json(project_file);
  if (!pj) return 1;
  auto project = logict::model::project_from_json(*pj);
  if (!project) {
    spdlog::critical("invalid project: {}", project.error().message);
    return 1;
  }

  logict::core::Runtime runtime(drivers);
  for (const auto& err : runtime.load(std::move(*project))) spdlog::error("{}", err);

  char host[256] = {};
  gethostname(host, sizeof(host) - 1);
#ifdef LOGICT_WITH_MQTT
  // Agents register their _IoT_Gateway tags, so they are prepared before the
  // runtime starts and before the OPC UA address space is built.
  logict::mqtt::MqttGateway mqtt(runtime, runtime.project().iot_gateway, host);
  for (const auto& err : mqtt.prepare()) spdlog::error("{}", err);
#endif

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  runtime.start();

#ifdef LOGICT_WITH_OPCUA
  std::unique_ptr<logict::opcua::OpcUaServer> opcua;
  auto ua_settings = logict::opcua::settings_from_json(config.value("opcua", nlohmann::json::object()));
  if (ua_settings.enabled) {
    std::vector<logict::opcua::UserAccount> users;
    for (const auto& u : config.value("users", nlohmann::json::array()))
      users.push_back({u.value("name", ""), u.value("password_hash", "")});
    opcua = std::make_unique<logict::opcua::OpcUaServer>(runtime, ua_settings, std::move(users));
    if (auto r = opcua->start(); !r) {
      spdlog::critical("OPC UA server failed to start: {}", r.error().message);
      runtime.stop();
      return 1;
    }
  }
#endif

#ifdef LOGICT_WITH_MQTT
  mqtt.start();
#endif

  logict::core::SubscriberId watcher = 0;
  if (watch_ms > 0) {
    auto& db = runtime.tags();
    watcher = db.add_subscriber([&db](logict::core::TagId id, const logict::sdk::Vqt& v, bool changed) {
      if (!changed) return;
      const auto& info = db.info(id);
      spdlog::info("{} = {} ({})", info.path, logict::sdk::to_string(v.value, &info.type),
                   logict::sdk::status_name(v.quality));
    });
    for (auto id : db.all())
      if (db.info(id).kind == logict::core::TagKind::Device) db.subscribe(id, watcher, watch_ms);
  }

  g_shutdown.acquire();
  spdlog::info("shutting down");
#ifdef LOGICT_WITH_MQTT
  mqtt.stop();
#endif
#ifdef LOGICT_WITH_OPCUA
  if (opcua) opcua->stop();
#endif
  if (watcher) runtime.tags().remove_subscriber(watcher);
  runtime.stop();
  return 0;
}
