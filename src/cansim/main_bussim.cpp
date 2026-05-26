#include "bus.hpp"
#include "bus_server.hpp"
#include "frame.hpp"
#include "wire_protocol.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace cansim;

static volatile std::sig_atomic_t g_stop = 0;

static void on_signal(int) { g_stop = 1; }

int main(int argc, char** argv) {
  std::string socket_path = default_socket_path();
  std::string vcan_iface = "vcan0";
  int bit_time_us = 1;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--socket" && i + 1 < argc) socket_path = argv[++i];
    else if (a == "--vcan" && i + 1 < argc) vcan_iface = argv[++i];
    else if (a == "--no-vcan") vcan_iface.clear();
    else if (a == "--bit-time-us" && i + 1 < argc) bit_time_us = std::atoi(argv[++i]);
    else if (a == "--quiet") quiet = true;
    else if (a == "--help" || a == "-h") {
      std::cout
          << "linuxcan-bussim — симулятор реальной CAN шины (битовый уровень).\n\n"
          << "Usage:\n"
          << "  linuxcan-bussim [--socket /tmp/linuxcan-bus.sock] [--vcan vcan0]\n"
          << "                  [--no-vcan] [--bit-time-us 1] [--quiet]\n\n"
          << "Каждое подключение через UDS = один CAN-контроллер.\n";
      return 0;
    } else {
      std::cerr << "Unknown argument: " << a << "\n";
      return 2;
    }
  }

  if (!frame_self_test()) {
    std::cerr << "bussim: frame_self_test() failed\n";
    return 2;
  }

  Bus bus;
  bus.set_bit_time_us(bit_time_us);

  BusServer server(bus, socket_path);
  if (!vcan_iface.empty()) server.set_vcan_mirror(vcan_iface);
  if (!server.start()) return 1;

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  if (!quiet) {
    std::cerr << "bussim: ready (socket=" << socket_path
              << ", vcan=" << (vcan_iface.empty() ? "off" : vcan_iface)
              << ", bit_time_us=" << bit_time_us << ")\n";
  }

  while (!g_stop) pause();
  server.stop();
  return 0;
}
