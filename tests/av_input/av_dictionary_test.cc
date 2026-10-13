#include "mw/streamer/ffmpeg/av_dictionary.h"

#include <catch2/catch_test_macros.hpp>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
#include <libavutil/dict.h>
}

namespace {

using mw::streamer::ffmpeg::AvDictionary;

struct NativeDictionary {
  ~NativeDictionary() { av_dict_free(&value); }
  AVDictionary* value = nullptr;
};

TEST_CASE(
    "AvDictionary distinguishes empty dictionaries, values and missing keys",
    "[av-dictionary]") {
  AvDictionary dictionary;
  CHECK(dictionary.get() == nullptr);
  CHECK(dictionary.ToMap().empty());
  CHECK_FALSE(dictionary.Get("missing").has_value());
  CHECK(AvDictionary(nullptr).ToMap().empty());
  CHECK(AvDictionary::ToMap(nullptr).empty());

  dictionary.Set("empty", "");
  const auto value = dictionary.Get("empty");
  REQUIRE(value.has_value());
  CHECK(value->empty());
  CHECK_FALSE(dictionary.Get("absent").has_value());
}

TEST_CASE("AvDictionary roundtrips maps and preserves exact-case Unicode keys",
          "[av-dictionary]") {
  const std::map<std::string, std::string> values{{"Token", "upper"},
                                                  {"token", "lower"},
                                                  {"来源", "直播源"},
                                                  {"empty", ""}};
  AvDictionary dictionary(values);
  CHECK(dictionary.ToMap() == values);
  CHECK(dictionary.Get("Token") == "upper");
  CHECK(dictionary.Get("token") == "lower");
  CHECK_FALSE(dictionary.Get("TOKEN").has_value());
  CHECK(dictionary.Get("来源") == "直播源");

  dictionary.Set("Token", "updated");
  CHECK(dictionary.Get("Token") == "updated");
  CHECK(dictionary.Get("token") == "lower");
  const AvDictionary& const_dictionary = dictionary;
  CHECK(av_dict_count(const_dictionary.get()) == 4);
  CHECK(AvDictionary::ToMap(const_dictionary.get()) == dictionary.ToMap());
}

TEST_CASE("AvDictionary deeply copies native dictionaries and other wrappers",
          "[av-dictionary][ownership]") {
  NativeDictionary native;
  REQUIRE(av_dict_set(&native.value, "source", "original", 0) == 0);
  AvDictionary original(native.value);
  REQUIRE(original.get() != native.value);
  av_dict_free(&native.value);
  CHECK(original.Get("source") == "original");

  AvDictionary copied(original);
  AvDictionary assigned;
  assigned.Set("obsolete", "old");
  assigned = original;
  REQUIRE(copied.get() != original.get());
  REQUIRE(assigned.get() != original.get());
  original.Set("source", "changed");
  CHECK(copied.Get("source") == "original");
  CHECK(assigned.Get("source") == "original");
  CHECK_FALSE(assigned.Get("obsolete").has_value());
  copied.Set("source", "copy");
  CHECK(original.Get("source") == "changed");
  CHECK(assigned.Get("source") == "original");
}

TEST_CASE(
    "AvDictionary moves native ownership and leaves reusable empty sources",
    "[av-dictionary][ownership]") {
  AvDictionary original;
  original.Set("source", "original");
  const auto* native = original.get();
  AvDictionary moved(std::move(original));
  CHECK(moved.get() == native);
  CHECK(original.get() == nullptr);
  original.Set("reused", "new");
  CHECK_FALSE(moved.Get("reused").has_value());

  AvDictionary assigned;
  assigned.Set("obsolete", "old");
  assigned = std::move(moved);
  CHECK(assigned.get() == native);
  CHECK(moved.get() == nullptr);
  CHECK(assigned.Get("source") == "original");
  CHECK_FALSE(assigned.Get("obsolete").has_value());
}

TEST_CASE("AvDictionary follows native updates and consumption through address",
          "[av-dictionary][interop]") {
  AvDictionary dictionary;
  REQUIRE(av_dict_set(dictionary.address(), "timeout", "1000", 0) == 0);
  REQUIRE(av_dict_set(dictionary.address(), "leftover", "value", 0) == 0);
  CHECK(dictionary.Get("timeout") == "1000");
  REQUIRE(av_dict_set(dictionary.address(), "timeout", nullptr, 0) == 0);
  CHECK_FALSE(dictionary.Get("timeout").has_value());
  CHECK(dictionary.Get("leftover") == "value");
  av_dict_free(dictionary.address());
  CHECK(dictionary.get() == nullptr);
  CHECK(dictionary.ToMap().empty());
  REQUIRE(av_dict_set(dictionary.address(), "replacement", "new", 0) == 0);
  CHECK(dictionary.Get("replacement") == "new");
}

TEST_CASE("AvDictionary rejects embedded NUL without truncating keys or values",
          "[av-dictionary][validation]") {
  AvDictionary dictionary;
  dictionary.Set("key", "original");
  const std::string_view key("key\0suffix", 10);
  const std::string_view value("value\0suffix", 12);
  CHECK_THROWS_AS(dictionary.Set(key, "changed"), std::invalid_argument);
  CHECK_THROWS_AS(dictionary.Set("key", value), std::invalid_argument);
  CHECK_THROWS_AS(dictionary.Get(key), std::invalid_argument);
  CHECK(dictionary.Get("key") == "original");
  CHECK_FALSE(dictionary.Get("value").has_value());
}

TEST_CASE("AvDictionary converts duplicate native keys using the first value",
          "[av-dictionary][interop]") {
  NativeDictionary native;
  REQUIRE(av_dict_set(&native.value, "key", "first", AV_DICT_MULTIKEY) == 0);
  REQUIRE(av_dict_set(&native.value, "key", "second", AV_DICT_MULTIKEY) == 0);
  REQUIRE(av_dict_count(native.value) == 2);
  const std::map<std::string, std::string> expected{{"key", "first"}};
  CHECK(AvDictionary::ToMap(native.value) == expected);
  AvDictionary copied(native.value);
  CHECK(av_dict_count(copied.get()) == 2);
  CHECK(copied.Get("key") == "first");
  CHECK(copied.ToMap() == expected);
}

}  // namespace
