#include "fmt/format.h"
#include "mw/streamer/version.h"

int main() {
  fmt::print("mw-streamer {}\n", mw::streamer::VersionString());
  return 0;
}
