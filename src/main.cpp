#include "ecu.hpp"
#include "ecu_config.hpp"
#include "encoding.hpp"
#include "http_server.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef LINUXCAN_WEB_DIR
#define LINUXCAN_WEB_DIR "."
#endif

namespace fs = std::filesystem;

static volatile std::sig_atomic_t g_stop = 0;

static void on_signal(int) { g_stop = 1; }

static std::string exe_dir() {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  return fs::path(buf).parent_path().string();
}

static std::string find_config_dir() {
  if (const char* env = std::getenv("LINUXCAN_CONFIG")) {
    if (fs::is_directory(env)) return env;
  }
  const std::vector<fs::path> candidates = {
      fs::current_path() / "config",
      fs::path(exe_dir()).parent_path() / "config",
      fs::path(LINUXCAN_WEB_DIR) / ".." / "config",
  };
  for (const auto& p : candidates) {
    if (fs::is_directory(p)) return fs::absolute(p).string();
  }
  return (fs::current_path() / "config").string();
}

static std::string find_web_root() {
  if (const char* env = std::getenv("LINUXCAN_WEB")) {
    if (fs::is_directory(env)) return env;
  }
  const std::vector<fs::path> candidates = {
      fs::current_path() / "web",
      fs::path(exe_dir()) / "web",
      fs::path(exe_dir()).parent_path() / "web",
      fs::path(LINUXCAN_WEB_DIR),
  };
  for (const auto& p : candidates) {
    if (fs::is_directory(p) && fs::exists(p / "index.html")) {
      return fs::absolute(p).string();
    }
  }
  return fs::absolute(LINUXCAN_WEB_DIR).string();
}

static void open_url(const std::string& url) {
  (void)std::system(("xdg-open '" + url + "' 2>/dev/null").c_str());
}

static int run_ecu(const std::string& ecu_id, const std::string& can_if,
                   const std::string& host, int port, const std::string& page,
                   const std::string& web_root, const std::string& config_dir,
                   bool quiet, bool open_ui) {
  if (!encoding_self_test()) {
    std::cerr << "FATAL: encoding self-test failed (float32)\n";
    return 2;
  }

  Ecu ecu(ecu_id, can_if);
  ecu.set_defaults(EcuDefaults::load(ecu_id, config_dir));
  ecu.set_config_dir(config_dir);
  ecu.set_ui_endpoint(port, page);
  if (const char* net = std::getenv("LINUXCAN_NETWORK_JSON")) {
    try {
      ecu.set_network(nlohmann::json::parse(net));
    } catch (...) {
      ecu.set_network(nlohmann::json::array());
    }
  }
  if (std::getenv("LINUXCAN_BUS_MONITOR")) {
    ecu.set_bus_monitor(true);
  }
  ecu.start();

  HttpServer server(ecu, web_root, host, port, page);
  if (!server.start()) {
    std::cerr << "ECU " << ecu_id << ": failed to start HTTP on port " << port << "\n";
    return 1;
  }

  const std::string ui_url = "http://" + host + ":" + std::to_string(port) + "/" + page;
  if (!quiet) {
    std::cerr << "ECU \"" << ecu_id << "\"\n"
              << "  CAN:  " << can_if << (ecu.status()["can_open"].get<bool>() ? " (ok)" : " (down)")
              << "\n"
              << "  UI:   " << ui_url << "\n";
  }
  if (open_ui) open_url(ui_url);

  while (!g_stop) pause();

  server.stop();
  ecu.stop();
  return 0;
}

int main(int argc, char** argv) {
  std::string ecu_id = "ecu";
  std::string can_if = "vcan0";
  std::string host = "127.0.0.1";
  int port = 8765;
  std::string page = "index.html";
  std::string config_dir;
  std::string network_json = "[]";
  bool bus_monitor = false;
  bool quiet = false;
  bool open_ui = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--ecu-id" && i + 1 < argc) ecu_id = argv[++i];
    else if (arg == "--can" && i + 1 < argc) can_if = argv[++i];
    else if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (arg == "--host" && i + 1 < argc) host = argv[++i];
    else if (arg == "--page" && i + 1 < argc) page = argv[++i];
    else if (arg == "--config-dir" && i + 1 < argc) config_dir = argv[++i];
    else if (arg == "--network-json" && i + 1 < argc) network_json = argv[++i];
    else if (arg == "--bus-monitor") bus_monitor = true;
    else if (arg == "--quiet" || arg == "--electron") quiet = true;
    else if (arg == "--open" || arg == "-o") open_ui = true;
    else if (arg == "--help" || arg == "-h") {
      std::cout
          << "linuxcan — один процесс ECU (свой CAN-контроллер + локальный UI)\n\n"
          << "Usage:\n"
          << "  linuxcan --ecu-id ID --port P --page ecu.html [--can vcan0]\n"
          << "           [--config-dir ecus/ID] [--network-json JSON] [--bus-monitor] [--open]\n\n"
          << "Без аргументов shell-launcher ./linuxcan запускает все ecus/*/ecu.json.\n";
      return 0;
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const std::string web_root = find_web_root();
  if (config_dir.empty()) config_dir = find_config_dir();
  setenv("LINUXCAN_NETWORK_JSON", network_json.c_str(), 1);
  if (bus_monitor) setenv("LINUXCAN_BUS_MONITOR", "1", 1);
  return run_ecu(ecu_id, can_if, host, port, page, web_root, config_dir, quiet, open_ui);
}
