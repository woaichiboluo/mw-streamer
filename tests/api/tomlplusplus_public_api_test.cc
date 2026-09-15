#include <string>

#include "mw/streamer.h"

int main() {
  const auto document = toml::parse("name = 'mw-streamer'");
  const auto name = document["name"].value<std::string>();
  return name && *name == "mw-streamer" ? 0 : 1;
}
