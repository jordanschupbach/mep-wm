#include "core/mep_theme.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

namespace mepwm::core {

namespace {

// Mirrors mep's src/persist.h MepAgentSocketDir() (also re-implemented in
// mep's own mcp/mep_client.ts::mepAgentSocketDir() for the same reason: no
// shared source between the two languages/repos, so both sides hand-port
// the same path rule independently).
std::string mep_agent_socket_dir() {
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
    return std::string(xdg) + "/mep/agent-sockets";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return {};
#ifdef __APPLE__
  return std::string(home) + "/Library/Application Support/mep/agent-sockets";
#else
  return std::string(home) + "/.local/share/mep/agent-sockets";
#endif
}

// Content-Length-framed JSON-RPC 2.0 request, matching mep's
// src/rpc_framing.h / mcp/rpc_framing.ts wire format exactly. `cmd` is
// always one of our own "colorscheme <name>" strings, never external
// input, so no JSON-escaping beyond quoting is needed.
std::string frame_command_run(const std::string& cmd) {
  const std::string body =
      R"({"jsonrpc":"2.0","id":1,"method":"command.run","params":{"cmd":")" + cmd + R"("}})";
  return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// Fire-and-forget: connects to one agent socket, writes the framed request,
// and closes without waiting for a response -- mep applies "colorscheme"
// synchronously on receipt, and nothing here needs the ack. Runs with a
// short send/connect timeout so a stale or wedged socket can't stall the
// detached thread indefinitely.
void send_to_socket(const std::string& socket_path, const std::string& frame) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return;

  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 300000;  // 300ms
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof addr.sun_path) {
    ::close(fd);
    return;
  }
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
    std::size_t written = 0;
    while (written < frame.size()) {
      const ssize_t sent = ::write(fd, frame.data() + written, frame.size() - written);
      if (sent <= 0) break;
      written += static_cast<std::size_t>(sent);
    }
  }
  ::close(fd);
}

}  // namespace

void sync_mep_theme(const std::string& mep_colorscheme_name) {
  if (mep_colorscheme_name.empty()) return;
  const std::string dir = mep_agent_socket_dir();
  if (dir.empty()) return;

  std::error_code error;
  if (!std::filesystem::is_directory(dir, error)) return;

  std::vector<std::string> sockets;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
    if (error) break;
    if (entry.path().extension() == ".sock") sockets.push_back(entry.path().string());
  }
  if (sockets.empty()) return;

  const std::string frame = frame_command_run("colorscheme " + mep_colorscheme_name);
  for (const std::string& socket_path : sockets) {
    std::thread([socket_path, frame] { send_to_socket(socket_path, frame); }).detach();
  }
}

}  // namespace mepwm::core
