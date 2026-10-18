#include "utils/slot_map.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <random>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

template<typename... Columns>
concept SlotMapCompatible = requires { sizeof(SlotMap<Columns...>); };

struct ThrowingMove {
   ThrowingMove() = default;
   ThrowingMove(ThrowingMove &&) noexcept(false) {}
};

struct ThrowingDestructor {
   ThrowingDestructor() = default;
   ThrowingDestructor(ThrowingDestructor &&) noexcept = default;
   ~ThrowingDestructor() noexcept(false) {}
};

struct IntMapTag {};
struct FirstMapTag {};
struct SecondMapTag {};

using IntMap = SlotMap<IntMapTag, Col<int>>;
using FirstTaggedIntMap = SlotMap<FirstMapTag, Col<int>>;
using SecondTaggedIntMap = SlotMap<SecondMapTag, Col<int>>;

template<typename Map, typename Handle>
concept AcceptsHandle = requires(Map &map, Handle handle) { map.get(handle); };

template<typename Handle>
constexpr std::uint64_t handle_value(Handle handle) noexcept {
   return static_cast<std::uint64_t>(handle);
}

static_assert(sizeof(IntMap::Handle) == 8);
static_assert(std::same_as<IntMap::Handle, SlotHandle<IntMapTag>>);
static_assert(SlotMapCompatible<IntMapTag, Col<int>>);
static_assert(SlotMapCompatible<FirstMapTag, Col<int>>);
static_assert(!SlotMapCompatible<Col<int>>);
static_assert(!SlotMapCompatible<int>);
static_assert(!SlotMapCompatible<IntMapTag, int>);
static_assert(!SlotMapCompatible<IntMapTag, Col<ThrowingMove>>);
static_assert(!SlotMapCompatible<IntMapTag, Col<ThrowingDestructor>>);
static_assert(std::same_as<IntMap::Handle, SlotMap<IntMapTag, Col<float>>::Handle>);
static_assert(!std::same_as<FirstTaggedIntMap::Handle, SecondTaggedIntMap::Handle>);
static_assert(AcceptsHandle<FirstTaggedIntMap, FirstTaggedIntMap::Handle>);
static_assert(!AcceptsHandle<FirstTaggedIntMap, SecondTaggedIntMap::Handle>);
static_assert(!std::convertible_to<IntMap::Handle, std::uint64_t>);
static_assert(std::constructible_from<IntMap::Handle, std::uint64_t>);

struct Record {
   int id;
   std::string name;

   Record(int pId, std::string pName) : id(pId), name(std::move(pName)) {}
};

struct RecordMapTag {};

static_assert(std::is_nothrow_move_constructible_v<Record>);

struct Tracked {
   static inline int alive = 0;
   static inline int copies = 0;
   static inline int moves = 0;

   int value;

   explicit Tracked(int pValue) : value(pValue) { ++alive; }

   Tracked(const Tracked &other) : value(other.value) {
      ++alive;
      ++copies;
   }

   Tracked(Tracked &&other) noexcept : value(other.value) {
      ++alive;
      ++moves;
   }

   Tracked &operator=(const Tracked &) = default;

   Tracked &operator=(Tracked &&other) noexcept {
      value = other.value;
      ++moves;
      return *this;
   }

   ~Tracked() { --alive; }

   static void reset() {
      alive = 0;
      copies = 0;
      moves = 0;
   }
};

struct TrackedMapTag {};

struct MoveConstructOnly {
   static inline int alive = 0;

   int value;

   explicit MoveConstructOnly(int pValue) : value(pValue) { ++alive; }

   MoveConstructOnly(const MoveConstructOnly &) = delete;
   MoveConstructOnly &operator=(const MoveConstructOnly &) = delete;

   MoveConstructOnly(MoveConstructOnly &&other) noexcept : value(other.value) { ++alive; }

   MoveConstructOnly &operator=(MoveConstructOnly &&) = delete;

   ~MoveConstructOnly() { --alive; }
};

struct MoveConstructOnlyMapTag {};

struct ThrowingConstructor {
   static inline bool shouldThrow = false;

   int value;

   explicit ThrowingConstructor(int pValue) : value(pValue) {
      if (shouldThrow) { throw std::runtime_error("expected construction failure"); }
   }

   ThrowingConstructor(ThrowingConstructor &&) noexcept = default;
};

struct ThrowingConstructorMapTag {};

struct Position {
   int value = 0;
};

struct Velocity {
   int value = 0;
};

struct CurrentPositionTag {};
struct PreviousPositionTag {};
struct EntityMapTag {};

using EntityMap = SlotMap<EntityMapTag, Col<Position, CurrentPositionTag>,
                          Col<Position, PreviousPositionTag>, Col<Velocity>>;

struct DefaultValue {
   int value = 42;
};

struct DefaultInsertionMapTag {};
struct RollbackMapTag {};

} // namespace

TEST_CASE("SlotMap starts empty", "[slot-map]") {
   IntMap map;

   CHECK(map.empty());
   CHECK(map.size() == 0);
   CHECK(map.slot_count() == 0);
   CHECK(map.values().empty());
   CHECK_FALSE(map.contains(IntMap::INVALID_HANDLE));
   CHECK(map.get(IntMap::INVALID_HANDLE) == nullptr);
}

TEST_CASE("SlotMap handles explicitly convert to and from uint64_t", "[slot-map]") {
   IntMap map;
   const auto handle = map.emplace(10);

   CHECK(handle_value(handle) == 0);
   CHECK(IntMap::Handle {handle_value(handle)} == handle);
}

TEST_CASE("Tagged SlotMaps expose distinct handle types", "[slot-map]") {
   FirstTaggedIntMap firstMap;
   SecondTaggedIntMap secondMap;

   const auto first = firstMap.emplace(10);
   const auto second = secondMap.emplace(20);

   CHECK(handle_value(first) == 0);
   CHECK(handle_value(second) == 0);
   CHECK(firstMap[first] == 10);
   CHECK(secondMap[second] == 20);
}

TEST_CASE("SlotMap emplace constructs one value per column", "[slot-map]") {
   SlotMap<RecordMapTag, Col<Record>> map;
   const auto handle = map.emplace(Record {7, "seven"});

   REQUIRE(map.get(handle) != nullptr);
   CHECK(map[handle].id == 7);
   CHECK(map[handle].name == "seven");
}

TEST_CASE("SlotMap insert copies lvalues and moves rvalues", "[slot-map]") {
   Tracked::reset();
   {
      SlotMap<TrackedMapTag, Col<Tracked>> map;
      map.reserve(2);

      Tracked copiedSource(10);
      const auto copiedHandle = map.insert(copiedSource);
      CHECK(Tracked::copies == 1);
      CHECK(Tracked::moves == 0);

      Tracked movedSource(20);
      const auto movedHandle = map.insert(std::move(movedSource));
      CHECK(Tracked::copies == 1);
      CHECK(Tracked::moves == 1);
      CHECK(map[copiedHandle].value == 10);
      CHECK(map[movedHandle].value == 20);
   }
   CHECK(Tracked::alive == 0);
}

TEST_CASE("SlotMap removal keeps values dense and repairs mappings", "[slot-map]") {
   IntMap map;
   const auto first = map.emplace(10);
   const auto middle = map.emplace(20);
   const auto last = map.emplace(30);

   REQUIRE(map.remove(middle));

   CHECK(map.size() == 2);
   CHECK(map.slot_count() == 3);
   CHECK(map.get(middle) == nullptr);
   REQUIRE(map.get(first) != nullptr);
   REQUIRE(map.get(last) != nullptr);
   CHECK(*map.get(first) == 10);
   CHECK(*map.get(last) == 30);

   const std::vector<int> expected {10, 30};
   CHECK(std::ranges::equal(map.values(), expected));
   CHECK(map.data() == map.values().data());
}

TEST_CASE("SlotMap removal supports values without move assignment", "[slot-map]") {
   MoveConstructOnly::alive = 0;
   {
      SlotMap<MoveConstructOnlyMapTag, Col<MoveConstructOnly>> map;
      map.reserve(3);
      const auto first = map.emplace(1);
      const auto middle = map.emplace(2);
      const auto last = map.emplace(3);

      REQUIRE(map.remove(middle));
      CHECK(map[first].value == 1);
      CHECK(map[last].value == 3);
      CHECK(MoveConstructOnly::alive == 2);
   }
   CHECK(MoveConstructOnly::alive == 0);
}

TEST_CASE("SlotMap freelist reuses sparse handles in LIFO order", "[slot-map]") {
   IntMap map;
   const auto first = map.emplace(1);
   const auto second = map.emplace(2);
   const auto third = map.emplace(3);
   const auto fourth = map.emplace(4);

   REQUIRE(map.remove(second));
   REQUIRE(map.remove(fourth));

   const auto reusedFourth = map.emplace(40);
   const auto reusedSecond = map.emplace(20);
   CHECK(reusedFourth == fourth);
   CHECK(reusedSecond == second);
   CHECK(map.slot_count() == 4);
   CHECK(map[first] == 1);
   CHECK(map[third] == 3);
   CHECK(map[reusedFourth] == 40);
   CHECK(map[reusedSecond] == 20);
}

TEST_CASE("SlotMap stale handles alias values after slot reuse", "[slot-map]") {
   IntMap map;
   const auto stale = map.emplace(1);
   REQUIRE(map.remove(stale));
   CHECK(map.get(stale) == nullptr);

   const auto reused = map.emplace(2);
   CHECK(reused == stale);
   CHECK(map.get(stale) == map.get(reused));
   CHECK(map[stale] == 2);
}

TEST_CASE("SlotMap rejects invalid and already removed handles", "[slot-map]") {
   IntMap map;
   const auto handle = map.emplace(1);

   CHECK_FALSE(map.remove(IntMap::Handle {handle_value(handle) + 1}));
   CHECK_FALSE(map.remove(IntMap::INVALID_HANDLE));
   REQUIRE(map.remove(handle));
   CHECK_FALSE(map.remove(handle));
   CHECK_FALSE(map.contains(handle));
   CHECK(map.get(handle) == nullptr);
}

TEST_CASE("SlotMap provides mutable and const single-column access", "[slot-map]") {
   IntMap map;
   const auto handle = map.emplace(10);
   map[handle] = 20;

   const IntMap &constMap = map;
   static_assert(std::is_same_v<decltype(constMap.get(handle)), const int *>);
   CHECK(constMap.contains(handle));
   CHECK(constMap.get(handle) != nullptr);
   CHECK(constMap[handle] == 20);
   CHECK(constMap.data() == map.data());
   CHECK(constMap.values().data() == map.values().data());
}

TEST_CASE("SlotMap reserve and clear manage all storage", "[slot-map]") {
   Tracked::reset();
   {
      SlotMap<TrackedMapTag, Col<Tracked>> map(32);
      CHECK(map.capacity() >= 32);
      const auto first = map.emplace(1);
      const auto second = map.emplace(2);
      CHECK(handle_value(first) == 0);
      CHECK(handle_value(second) == 1);
      CHECK(Tracked::alive == 2);

      map.clear();
      CHECK(map.empty());
      CHECK(map.slot_count() == 0);
      CHECK(Tracked::alive == 0);
      CHECK(handle_value(map.emplace(3)) == 0);
   }
   CHECK(Tracked::alive == 0);
}

TEST_CASE("SlotMap remains usable after moves", "[slot-map]") {
   IntMap source;
   const auto first = source.emplace(10);
   const auto freeHandle = source.emplace(20);
   const auto last = source.emplace(30);
   REQUIRE(source.remove(freeHandle));

   int *firstPointer = source.get(first);
   auto sourceValues = source.values();

   IntMap moved(std::move(source));
   CHECK(source.empty());
   CHECK(source.slot_count() == 0);
   CHECK(handle_value(source.emplace(1)) == 0);
   CHECK(moved[first] == 10);
   CHECK(moved[last] == 30);
   CHECK(firstPointer == moved.get(first));
   CHECK(sourceValues.data() == moved.data());
   CHECK(moved.emplace(40) == freeHandle);

   IntMap assigned;
   const auto discarded = assigned.emplace(99);
   CHECK(handle_value(discarded) == 0);

   int *movedFirstPointer = moved.get(first);
   assigned = std::move(moved);
   CHECK(moved.empty());
   CHECK(moved.slot_count() == 0);
   CHECK(assigned[first] == 10);
   CHECK(assigned[last] == 30);
   CHECK(assigned[freeHandle] == 40);
   CHECK(movedFirstPointer == assigned.get(first));
}

TEST_CASE("SlotMap rolls back when single-column construction throws", "[slot-map]") {
   SlotMap<ThrowingConstructorMapTag, Col<ThrowingConstructor>> map;
   map.reserve(2);
   const auto retained = map.emplace(7);

   ThrowingConstructor::shouldThrow = true;
   CHECK_THROWS_AS((void)map.emplace(8), std::runtime_error);
   ThrowingConstructor::shouldThrow = false;

   CHECK(map.size() == 1);
   CHECK(map.slot_count() == 1);
   REQUIRE(map.get(retained) != nullptr);
   CHECK(map[retained].value == 7);
}

TEST_CASE("SlotMap supports duplicate value types through distinct tags", "[slot-map][soa]") {
   EntityMap map;
   const auto handle = map.emplace(Position {1}, Position {10}, Velocity {100});

   REQUIRE(map.get<CurrentPositionTag>(handle) != nullptr);
   REQUIRE(map.get<PreviousPositionTag>(handle) != nullptr);
   REQUIRE(map.get<Velocity>(handle) != nullptr);
   CHECK(map.get<CurrentPositionTag>(handle)->value == 1);
   CHECK(map.get<PreviousPositionTag>(handle)->value == 10);
   CHECK(map.get<Velocity>(handle)->value == 100);

   CHECK(map.get<0>(handle) == map.get<CurrentPositionTag>(handle));
   CHECK(map.get<1>(handle) == map.get<PreviousPositionTag>(handle));
   CHECK(map.get<2>(handle) == map.get<Velocity>(handle));

   auto [current, previous, velocity] = map.row(handle);
   current.value = 2;
   previous.value = 20;
   velocity.value = 200;
   CHECK(map.get<CurrentPositionTag>(handle)->value == 2);
   CHECK(map.get<PreviousPositionTag>(handle)->value == 20);
   CHECK(map.get<Velocity>(handle)->value == 200);
}

TEST_CASE("SlotMap keeps every column synchronized during removal", "[slot-map][soa]") {
   EntityMap map;
   const auto first = map.emplace(Position {1}, Position {11}, Velocity {101});
   const auto middle = map.emplace(Position {2}, Position {12}, Velocity {102});
   const auto last = map.emplace(Position {3}, Position {13}, Velocity {103});

   REQUIRE(map.remove(middle));

   CHECK(map.size() == 2);
   CHECK(map.get<CurrentPositionTag>(middle) == nullptr);
   CHECK(map.get<CurrentPositionTag>(first)->value == 1);
   CHECK(map.get<PreviousPositionTag>(first)->value == 11);
   CHECK(map.get<Velocity>(first)->value == 101);
   CHECK(map.get<CurrentPositionTag>(last)->value == 3);
   CHECK(map.get<PreviousPositionTag>(last)->value == 13);
   CHECK(map.get<Velocity>(last)->value == 103);

   auto [current, previous, velocity] = map.columns();
   CHECK(current.size() == 2);
   CHECK(previous.size() == 2);
   CHECK(velocity.size() == 2);
   CHECK(current[0].value == 1);
   CHECK(current[1].value == 3);
   CHECK(previous[0].value == 11);
   CHECK(previous[1].value == 13);
   CHECK(velocity[0].value == 101);
   CHECK(velocity[1].value == 103);

   CHECK(map.column<CurrentPositionTag>().data() == map.column<0>().data());
   CHECK(map.column<PreviousPositionTag>().data() == map.column<1>().data());
   CHECK(map.column<Velocity>().data() == map.column<2>().data());
}

TEST_CASE("SlotMap default insertion initializes every column", "[slot-map][soa]") {
   SlotMap<DefaultInsertionMapTag, Col<int>, Col<DefaultValue>> map;
   const auto emplaced = map.emplace();
   const auto inserted = map.insert();

   CHECK(*map.get<int>(emplaced) == 0);
   CHECK(map.get<DefaultValue>(emplaced)->value == 42);
   CHECK(*map.get<int>(inserted) == 0);
   CHECK(map.get<DefaultValue>(inserted)->value == 42);
}

TEST_CASE("SlotMap rolls back completed columns when later construction throws",
          "[slot-map][soa]") {
   SlotMap<RollbackMapTag, Col<int>, Col<ThrowingConstructor>> map;
   const auto retained = map.emplace(7, 70);

   ThrowingConstructor::shouldThrow = true;
   CHECK_THROWS_AS((void)map.emplace(8, 80), std::runtime_error);
   ThrowingConstructor::shouldThrow = false;

   CHECK(map.size() == 1);
   CHECK(map.slot_count() == 1);
   CHECK(map.column<int>().size() == 1);
   CHECK(map.column<ThrowingConstructor>().size() == 1);
   CHECK(*map.get<int>(retained) == 7);
   CHECK(map.get<ThrowingConstructor>(retained)->value == 70);
}

TEST_CASE("SlotMap maintains mappings under randomized operations", "[slot-map][stress]") {
   using Map = IntMap;
   Map map;
   std::vector<std::pair<Map::Handle, int>> active;
   std::mt19937 random(0x51A7u);
   int nextValue = 1;

   for (int operation = 0; operation < 5000; ++operation) {
      CAPTURE(operation);
      if (active.empty() || (random() & 1u) != 0) {
         const int value = nextValue++;
         const auto handle = map.emplace(value);
         CHECK(std::ranges::none_of(active,
                                    [handle](const auto &entry) { return entry.first == handle; }));
         active.emplace_back(handle, value);
      } else {
         const auto index = static_cast<std::size_t>(random()) % active.size();
         REQUIRE(map.remove(active[index].first));
         active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
      }

      REQUIRE(map.size() == active.size());
      for (const auto &[handle, value] : active) {
         REQUIRE(map.get(handle) != nullptr);
         CHECK(*map.get(handle) == value);
      }
      CHECK(static_cast<std::size_t>(std::ranges::distance(map.values())) == active.size());
   }
}
