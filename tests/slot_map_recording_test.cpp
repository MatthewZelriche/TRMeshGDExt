#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>
#include <ranges>
#include <utility>
#include <vector>

#include "utils/slot_map.hpp"

namespace {

struct RecordingIntMapTag {};
struct RecordingValueTag {};

using RecordingIntMap = SlotMap<RecordingIntMapTag, Col<int>, Col<float, RecordingValueTag>>;

std::vector<int> integers(const RecordingIntMap &map) {
   const auto values = std::get<0>(map.columns());
   return {values.begin(), values.end()};
}

std::vector<std::pair<std::uint64_t, int>> state(const RecordingIntMap &map) {
   std::vector<std::pair<std::uint64_t, int>> result;
   for (std::size_t dense_index = 0; dense_index < map.size(); ++dense_index) {
      const RecordingIntMap::Handle handle = map.handle_at(dense_index);
      const auto value = static_cast<std::uint64_t>(handle);
      result.emplace_back(value, *map.get<0>(handle));
   }
   return result;
}

std::vector<RecordingIntMap::Handle> random_operation(RecordingIntMap &map, std::uint32_t seed,
                                                      int steps) {
   std::mt19937 random(seed);
   std::vector<RecordingIntMap::Handle> inserted;

   for (int step = 0; step < steps; ++step) {
      std::vector<RecordingIntMap::Handle> live;
      for (std::uint64_t value = 0; value < map.slot_count(); ++value) {
         const RecordingIntMap::Handle handle {value};
         if (map.contains(handle)) { live.push_back(handle); }
      }

      const std::uint32_t choice = random() % 3;
      if (choice == 0 || live.empty()) {
         inserted.push_back(
             map.emplace(static_cast<int>(random()), static_cast<float>(random() % 1000)));
      } else if (choice == 1) {
         REQUIRE(map.remove(live[random() % live.size()]));
      } else {
         *map.get_mut<0>(live[random() % live.size()]) = static_cast<int>(random());
      }
   }

   return inserted;
}

} // namespace

TEST_CASE("SlotMap recording reverts allocator state and deterministically replays handles",
          "[recording][slot-map]") {
   RecordingIntMap map;
   const auto first = map.emplace(10, 1.0F);
   const auto second = map.emplace(20, 2.0F);
   const auto third = map.emplace(30, 3.0F);
   const auto fourth = map.emplace(40, 4.0F);
   REQUIRE(map.remove(second));
   REQUIRE(map.remove(fourth));

   map.begin_recording();
   const auto reused_fourth = map.emplace(41, 4.1F);
   const auto reused_second = map.emplace(21, 2.1F);
   const auto grown = map.emplace(50, 5.0F);
   *map.get_mut<0>(first) = 11;
   REQUIRE(map.remove(reused_fourth));
   const auto reused_again = map.emplace(42, 4.2F);
   RecordingIntMap::Patch patch = map.commit_recording();

   const std::vector<int> edited = integers(map);
   CHECK(reused_fourth == fourth);
   CHECK(reused_second == second);
   CHECK(reused_again == fourth);

   map.revert(patch);
   CHECK(integers(map) == (std::vector<int> {10, 30}));
   CHECK(map.contains(first));
   CHECK(map.contains(third));
   CHECK_FALSE(map.contains(second));
   CHECK_FALSE(map.contains(fourth));
   CHECK_FALSE(map.contains(grown));

   map.begin_recording();
   const auto replay_fourth = map.emplace(41, 4.1F);
   const auto replay_second = map.emplace(21, 2.1F);
   const auto replay_grown = map.emplace(50, 5.0F);
   *map.get_mut<0>(first) = 11;
   REQUIRE(map.remove(replay_fourth));
   const auto replay_again = map.emplace(42, 4.2F);
   [[maybe_unused]] RecordingIntMap::Patch replay_patch = map.commit_recording();

   CHECK(replay_fourth == reused_fourth);
   CHECK(replay_second == reused_second);
   CHECK(replay_grown == grown);
   CHECK(replay_again == reused_again);
   CHECK(integers(map) == edited);
}

TEST_CASE("SlotMap recording handles a grown slot removed and reinserted in one recording",
          "[recording][slot-map]") {
   RecordingIntMap map;
   const auto retained = map.emplace(1, 1.0F);

   map.begin_recording();
   const auto grown = map.emplace(2, 2.0F);
   [[maybe_unused]] const auto later = map.emplace(3, 3.0F);
   REQUIRE(map.remove(grown));
   CHECK(map.emplace(4, 4.0F) == grown);
   RecordingIntMap::Patch patch = map.commit_recording();

   map.revert(patch);
   CHECK(map.size() == 1);
   CHECK(map.slot_count() == 1);
   CHECK(*map.get<0>(retained) == 1);
   CHECK(map.emplace(5, 5.0F) == grown);
}

TEST_CASE("SlotMap recording saves a repeatedly written row only once", "[recording][slot-map]") {
   RecordingIntMap map;
   const auto handle = map.emplace(1, 1.0F);

   map.begin_recording();
   for (int value = 2; value < 8; ++value) { *map.get_mut<0>(handle) = value; }
   RecordingIntMap::Patch repeated_patch = map.commit_recording();

   map.revert(repeated_patch);
   map.begin_recording();
   *map.get_mut<0>(handle) = 9;
   RecordingIntMap::Patch single_patch = map.commit_recording();

   CHECK(repeated_patch.byte_size() == single_patch.byte_size());
   map.revert(single_patch);
   CHECK(*map.get<0>(handle) == 1);
}

TEST_CASE("SlotMap recording captures mutations through bulk column access",
          "[recording][slot-map]") {
   RecordingIntMap map;
   const auto first = map.emplace(1, 1.0F);
   const auto second = map.emplace(2, 2.0F);

   map.begin_recording();
   auto [integer_column, float_column] = map.columns_mut();
   integer_column[0] = 10;
   float_column[1] = 20.0F;
   RecordingIntMap::Patch patch = map.commit_recording();

   map.revert(patch);
   CHECK(*map.get<0>(first) == 1);
   CHECK(*map.get<RecordingValueTag>(first) == 1.0F);
   CHECK(*map.get<0>(second) == 2);
   CHECK(*map.get<RecordingValueTag>(second) == 2.0F);
}

TEST_CASE("SlotMap recording randomized edits revert and replay identically",
          "[recording][slot-map][stress]") {
   for (std::uint32_t seed = 1; seed <= 8; ++seed) {
      CAPTURE(seed);
      RecordingIntMap map;
      [[maybe_unused]] const auto seeded = random_operation(map, seed, 40);
      const auto before = state(map);

      map.begin_recording();
      const auto first_handles = random_operation(map, seed * 100, 80);
      RecordingIntMap::Patch patch = map.commit_recording();
      const auto after = state(map);

      map.revert(patch);
      REQUIRE(state(map) == before);

      map.begin_recording();
      const auto replay_handles = random_operation(map, seed * 100, 80);
      [[maybe_unused]] RecordingIntMap::Patch replay_patch = map.commit_recording();
      CHECK(replay_handles == first_handles);
      CHECK(state(map) == after);
   }
}
