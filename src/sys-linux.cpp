#include <unistd.h>

#include <array>

#include "remy/sys.hpp"

namespace remy {

std::string get_hostname() {
  std::array<char, 256> hostname_buffer;
  hostname_buffer.fill(0);

  if (::gethostname(hostname_buffer.data(), hostname_buffer.size()) == 0) {
    return {hostname_buffer.data()};
  }
  return "";
}

}  // namespace remy
