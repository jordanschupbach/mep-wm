#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc != 3 || std::string(argv[1]) != "-m") {
    std::cerr << "usage: mep-wm-cli -m <lua>\n";
    return 2;
  }
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  const char* display = std::getenv("DISPLAY");
  const std::string path = std::string(runtime && *runtime ? runtime : "/tmp") + "/mep-wm-" +
                           (display && *display ? display : "display") + ".sock";
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    std::cerr << "socket path is too long\n";
    return 2;
  }
  std::strcpy(address.sun_path, path.c_str());
  const int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0 || connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::perror("mep-wm-cli");
    return 1;
  }
  const std::string source = argv[2];
  if (write(socket_fd, source.data(), source.size()) < 0) return 1;
  shutdown(socket_fd, SHUT_WR);
  char buffer[4096];
  ssize_t count;
  while ((count = read(socket_fd, buffer, sizeof(buffer))) > 0) std::cout.write(buffer, count);
  close(socket_fd);
  return 0;
}
