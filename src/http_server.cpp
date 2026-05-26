#include "http_server.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <vector>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

std::string base64_encode(const std::string& in) {
  static const char* tbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(tbl[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string sha1(const std::string& msg) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  std::vector<uint8_t> data(msg.begin(), msg.end());
  const uint64_t bit_len = data.size() * 8;
  data.push_back(0x80);
  while ((data.size() % 64) != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>((bit_len >> (8 * i)) & 0xff));

  auto rol = [](uint32_t v, uint32_t bits) { return (v << bits) | (v >> (32 - bits)); };

  for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(data[chunk + i * 4]) << 24) |
             (static_cast<uint32_t>(data[chunk + i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(data[chunk + i * 4 + 2]) << 8) |
             static_cast<uint32_t>(data[chunk + i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::string out(20, '\0');
  const uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<char>((hs[i] >> 24) & 0xff);
    out[i * 4 + 1] = static_cast<char>((hs[i] >> 16) & 0xff);
    out[i * 4 + 2] = static_cast<char>((hs[i] >> 8) & 0xff);
    out[i * 4 + 3] = static_cast<char>(hs[i] & 0xff);
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \r\n\t");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \r\n\t");
  return s.substr(b, e - b + 1);
}

}  // namespace

HttpServer::HttpServer(Ecu& ecu, std::string web_root, std::string host, int port,
                       std::string default_page)
    : ecu_(ecu),
      web_root_(std::move(web_root)),
      host_(std::move(host)),
      port_(port),
      default_page_(std::move(default_page)) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return false;

  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "bind failed: " << std::strerror(errno) << std::endl;
    return false;
  }
  if (listen(listen_fd_, 32) < 0) return false;

  ecu_.set_send_fn([this](int fd, const nlohmann::json& msg) { ws_send_text(fd, msg.dump()); });

  running_ = true;
  accept_thread_ = std::thread([this] { accept_loop(); });
  std::cerr << "HTTP/WS listening on http://" << host_ << ":" << port_ << "/" << std::endl;
  return true;
}

void HttpServer::stop() {
  running_ = false;
  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
}

void HttpServer::accept_loop() {
  while (running_) {
    const int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    std::thread([this, fd] {
      handle_client(fd);
      close(fd);
    }).detach();
  }
}

void HttpServer::handle_client(int fd) {
  const std::string req = read_request(fd);
  if (req.empty()) return;

  if (req.find("Upgrade: websocket") != std::string::npos ||
      req.find("upgrade: websocket") != std::string::npos) {
    if (upgrade_websocket(fd, req)) {
      ecu_.attach_ui(fd);
      ws_loop(fd);
      ecu_.detach_ui(fd);
    }
    return;
  }
  serve_http(fd, req);
}

std::string HttpServer::read_request(int fd) {
  std::string buf;
  char chunk[4096];
  while (buf.find("\r\n\r\n") == std::string::npos) {
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    buf.append(chunk, chunk + n);
    if (buf.size() > 65536) break;
  }
  return buf;
}

bool HttpServer::upgrade_websocket(int fd, const std::string& request) {
  std::string key;
  std::istringstream ss(request);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.rfind("Sec-WebSocket-Key:", 0) == 0 ||
        line.rfind("sec-websocket-key:", 0) == 0) {
      key = trim(line.substr(line.find(':') + 1));
    }
  }
  if (key.empty()) return false;

  const std::string accept = base64_encode(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
  std::ostringstream resp;
  resp << "HTTP/1.1 101 Switching Protocols\r\n"
       << "Upgrade: websocket\r\n"
       << "Connection: Upgrade\r\n"
       << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
  const auto s = resp.str();
  send(fd, s.data(), s.size(), 0);
  return true;
}

void HttpServer::ws_send_text(int fd, const std::string& text) {
  std::vector<uint8_t> frame;
  frame.push_back(0x81);
  if (text.size() < 126) {
    frame.push_back(static_cast<uint8_t>(text.size()));
  } else {
    frame.push_back(126);
    frame.push_back(static_cast<uint8_t>((text.size() >> 8) & 0xff));
    frame.push_back(static_cast<uint8_t>(text.size() & 0xff));
  }
  frame.insert(frame.end(), text.begin(), text.end());
  send(fd, frame.data(), frame.size(), 0);
}

std::string HttpServer::ws_recv_frame(int fd) {
  uint8_t hdr[2];
  if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return "";
  const uint8_t opcode = hdr[0] & 0x0f;
  if (opcode == 0x8) return "";  // close

  uint64_t len = hdr[1] & 0x7f;
  if (len == 126) {
    uint8_t ext[2];
    if (recv(fd, ext, 2, MSG_WAITALL) != 2) return "";
    len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    uint8_t ext[8];
    if (recv(fd, ext, 8, MSG_WAITALL) != 8) return "";
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
  }

  const bool masked = (hdr[1] & 0x80) != 0;
  uint8_t mask[4] = {};
  if (masked && recv(fd, mask, 4, MSG_WAITALL) != 4) return "";

  std::string payload(len, '\0');
  size_t got = 0;
  while (got < len) {
    const ssize_t n = recv(fd, payload.data() + got, static_cast<size_t>(len - got), 0);
    if (n <= 0) return "";
    got += static_cast<size_t>(n);
  }
  if (masked) {
    for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
  }
  return payload;
}

void HttpServer::ws_loop(int fd) {
  bool registered = false;
  while (true) {
    const std::string frame = ws_recv_frame(fd);
    if (frame.empty()) break;

    nlohmann::json data;
    try {
      data = nlohmann::json::parse(frame);
    } catch (...) {
      ws_send_text(fd, R"({"type":"error","message":"Invalid JSON"})");
      continue;
    }

    const std::string type = data.value("type", "");
    try {
      if (type == "register") {
        ecu_.register_ui(fd);
        registered = true;
      } else if (type == "send_config" || type == "tx_config") {
        if (!registered) throw std::runtime_error("Register first");
        ecu_.set_tx_config(fd, data.contains("profile") ? data["profile"] : data);
        ws_send_text(fd, R"({"type":"ok"})");
      } else if (type == "receive_config" || type == "rx_config") {
        if (!registered) throw std::runtime_error("Register first");
        ecu_.set_rx_config(fd, data.contains("profile") ? data["profile"] : data);
        ws_send_text(fd, R"({"type":"ok"})");
      } else if (type == "publish") {
        if (!registered) throw std::runtime_error("Register first");
        ecu_.publish(fd, data.value("payload", nlohmann::json::object()));
      } else if (type == "stage_payload") {
        if (!registered) throw std::runtime_error("Register first");
        ecu_.stage_payload(fd, data.value("payload", nlohmann::json::object()));
        ws_send_text(fd, R"({"type":"ok"})");
      } else if (type == "reset_bus_off") {
        if (!registered) throw std::runtime_error("Register first");
        ecu_.reset_bus_off();
        ws_send_text(fd, R"({"type":"ok"})");
      } else {
        ws_send_text(fd, nlohmann::json{{"type", "error"}, {"message", "Unknown type"}}.dump());
      }
    } catch (const std::exception& e) {
      ws_send_text(fd, nlohmann::json{{"type", "error"}, {"message", e.what()}}.dump());
    }
  }
}

static bool ends_with(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

static std::string strip_url_suffix(std::string path) {
  const size_t q = path.find_first_of("?#");
  if (q != std::string::npos) path.resize(q);
  return path;
}

std::string HttpServer::mime_type(const std::string& path) const {
  if (ends_with(path, ".html")) return "text/html; charset=utf-8";
  if (ends_with(path, ".js")) return "application/javascript; charset=utf-8";
  if (ends_with(path, ".css")) return "text/css; charset=utf-8";
  return "application/octet-stream";
}

std::string HttpServer::resolve_path(const std::string& url_path) const {
  std::string p = strip_url_suffix(url_path);
  if (p == "/" || p.empty()) p = "/" + default_page_;
  if (p[0] == '/') p = p.substr(1);
  return web_root_ + "/" + p;
}

bool HttpServer::serve_ecu_config(int fd) const {
  const std::string body = ecu_.status().dump(2);
  std::ostringstream resp;
  resp << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << body;
  const auto s = resp.str();
  send(fd, s.data(), s.size(), 0);
  return true;
}

bool HttpServer::serve_network(int fd) const {
  const auto status = ecu_.status();
  const std::string body = status.value("network", nlohmann::json::array()).dump(2);
  std::ostringstream resp;
  resp << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << body;
  const auto s = resp.str();
  send(fd, s.data(), s.size(), 0);
  return true;
}

bool HttpServer::serve_http(int fd, const std::string& request) {
  std::string method, path;
  {
    std::istringstream ss(request);
    ss >> method >> path;
  }
  if (method != "GET") {
    const char* resp = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
    send(fd, resp, std::strlen(resp), 0);
    return true;
  }

  path = strip_url_suffix(path);
  if (path == "/ecu-config.json") return serve_ecu_config(fd);
  if (path == "/network.json") return serve_network(fd);

  const std::string file_path = resolve_path(path);
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    send(fd, resp, std::strlen(resp), 0);
    return true;
  }

  std::ostringstream body;
  body << in.rdbuf();
  const std::string content = body.str();
  std::ostringstream resp;
  resp << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: " << mime_type(file_path) << "\r\n"
       << "Content-Length: " << content.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << content;
  const auto s = resp.str();
  send(fd, s.data(), s.size(), 0);
  return true;
}
