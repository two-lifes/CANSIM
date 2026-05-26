#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "ecu.hpp"

class HttpServer {
 public:
  HttpServer(Ecu& ecu, std::string web_root, std::string host, int port,
             std::string default_page = "index.html");
  ~HttpServer();

  bool start();
  void stop();

 private:
  void accept_loop();
  void handle_client(int fd);
  bool serve_http(int fd, const std::string& request);
  bool upgrade_websocket(int fd, const std::string& request);
  void ws_loop(int fd);
  std::string read_request(int fd);
  void ws_send_text(int fd, const std::string& text);
  std::string ws_recv_frame(int fd);
  std::string mime_type(const std::string& path) const;
  std::string resolve_path(const std::string& url_path) const;
  bool serve_ecu_config(int fd) const;
  bool serve_network(int fd) const;

  Ecu& ecu_;
  std::string web_root_;
  std::string host_;
  int port_;
  std::string default_page_;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
};
