#include <array>
#include <cstdint>

extern "C" {
#include "mov-internal.h"
}

#include <catch2/catch_test_macros.hpp>

TEST_CASE("fMP4 fragment seek maps movie time through the edit list") {
  std::array<mov_fragment_t, 3> fragments{
      {{0, 100}, {48000, 200}, {96000, 300}}};
  std::array<mov_elst_t, 2> edits{};
  mov_track_t track{};
  track.mdhd.timescale = 48000;
  track.frags = fragments.data();
  track.frag_count = fragments.size();
  track.elst = edits.data();
  track.elst_count = 1;
  edits[0] = {3000, 4800, 1, 0};
  mov_t movie{};
  movie.mvhd.timescale = 1000;
  movie.tracks = &track;
  movie.track_count = 1;
  std::int64_t requested = -100;
  std::int64_t expected = -100;
  std::uint32_t expected_fragment = 0;

  SECTION("negative decode preroll selects the first fragment") {}
  SECTION("media edit is removed from an ordinary seek request") {
    requested = 900;
    expected = 900;
    expected_fragment = 1;
  }
  SECTION("leading empty edit and media edit share the same mapping") {
    edits[0] = {500, -1, 1, 0};
    edits[1] = {3000, 4800, 1, 0};
    track.elst_count = 2;
    requested = 1400;
    expected = 1400;
    expected_fragment = 1;
  }
  SECTION("request before the edited media start selects the first fragment") {
    edits[0] = {500, -1, 1, 0};
    edits[1] = {3000, 4800, 1, 0};
    track.elst_count = 2;
    requested = -200;
    expected = 400;
  }
  SECTION("unedited negative request cannot wrap to the last fragment") {
    track.elst_count = 0;
    requested = -1;
    expected = 0;
  }
  REQUIRE(mov_fragment_seek(&movie, &requested) == 0);
  CHECK(requested == expected);
  CHECK(track.frag_capacity == expected_fragment);
}
