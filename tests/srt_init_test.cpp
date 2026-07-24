#include <catch2/catch_test_macros.hpp>

#include "mw/init/init.hpp"
#include "srt/SrtEpollReactor.h"

TEST_CASE("shutdown does not create an unused SRT reactor",
          "[init][srt][lifecycle]") {
  CHECK_FALSE(mediakit::SrtEpollReactor::isCreated());
  mw::shutdown();
  CHECK_FALSE(mediakit::SrtEpollReactor::isCreated());
}

TEST_CASE("shutdown stops an existing SRT reactor", "[init][srt][lifecycle]") {
  auto& reactor = mediakit::SrtEpollReactor::Instance();
  REQUIRE(mediakit::SrtEpollReactor::isCreated());
  REQUIRE(reactor.available());

  mw::shutdown();

  CHECK(mediakit::SrtEpollReactor::isCreated());
  CHECK_FALSE(reactor.available());
  CHECK_THROWS_AS(mw::init(), std::logic_error);
}
