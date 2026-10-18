#include "utils/bump_allocator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <ranges>
#include <stdexcept>

namespace {

struct DefaultValue {
   std::uint64_t value = 42;
};

struct alignas(64) OverAlignedValue {
   std::uint64_t value = 7;
};

struct ThrowingDefault {
   static inline int constructions = 0;

   ThrowingDefault() {
      if (++constructions == 2) { throw std::runtime_error("construction failed"); }
   }

   std::uint32_t value = 0;
};

struct NonThrowingDefault {
   std::uint32_t value = 0;
};

static_assert(std::is_trivially_destructible_v<DefaultValue>);
static_assert(std::is_trivially_destructible_v<OverAlignedValue>);
static_assert(std::is_trivially_destructible_v<ThrowingDefault>);
static_assert(std::is_nothrow_move_constructible_v<BumpAllocator>);
static_assert(std::is_nothrow_move_assignable_v<BumpAllocator>);

} // namespace

TEST_CASE("BumpAllocator value-initializes allocated spans") {
   BumpAllocator allocator(128);

   const auto integers = allocator.allocate_span<int>(4);
   REQUIRE(std::ranges::all_of(integers, [](int value) { return value == 0; }));

   const auto values = allocator.allocate_span<DefaultValue>(3);
   REQUIRE(std::ranges::all_of(values,
                               [](const DefaultValue &value) { return value.value == 42; }));
}

TEST_CASE("BumpAllocator value-initializes individual objects") {
   BumpAllocator allocator(128);

   int *integer = allocator.create<int>();
   DefaultValue *value = allocator.create<DefaultValue>();

   REQUIRE(*integer == 0);
   REQUIRE(value->value == 42);
}

TEST_CASE("BumpAllocator supports aligned overflow allocations") {
   BumpAllocator allocator(16);

   const auto values = allocator.allocate_span<OverAlignedValue>(2);

   REQUIRE(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(OverAlignedValue) == 0);
   REQUIRE(std::ranges::all_of(
       values, [](const OverAlignedValue &value) { return value.value == 7; }));
}

TEST_CASE("BumpAllocator reset reuses its first block") {
   BumpAllocator allocator(128);

   const auto first = allocator.allocate_span<std::uint64_t>(4);
   const auto *first_address = first.data();

   allocator.reset();

   const auto second = allocator.allocate_span<std::uint64_t>(4);
   REQUIRE(second.data() == first_address);
}

TEST_CASE("BumpAllocator restores a checkpoint without releasing its blocks") {
   BumpAllocator allocator(64);

   const auto retained = allocator.allocate_span<std::uint64_t>(2);
   retained[0] = 11;
   const auto saved = allocator.checkpoint();

   const auto temporary = allocator.allocate_span<std::uint64_t>(4);
   const auto *temporary_address = temporary.data();

   allocator.restore(saved);

   const auto replacement = allocator.allocate_span<std::uint64_t>(4);
   REQUIRE(replacement.data() == temporary_address);
   REQUIRE(retained[0] == 11);
}

TEST_CASE("BumpAllocator moves without relocating existing allocations") {
   BumpAllocator source(64);

   const auto retained = source.allocate_span<std::uint64_t>(2);
   retained[0] = 17;
   const auto saved = source.checkpoint();
   const auto temporary = source.allocate_span<std::uint64_t>(4);
   const auto *temporary_address = temporary.data();

   BumpAllocator destination(std::move(source));

   REQUIRE(retained[0] == 17);
   destination.restore(saved);
   REQUIRE(destination.allocate_span<std::uint64_t>(4).data() == temporary_address);

   REQUIRE(source.allocate_span<std::uint64_t>(1)[0] == 0);
}

TEST_CASE("BumpAllocator rewinds when element construction throws") {
   BumpAllocator allocator(128);

   const auto probe = allocator.allocate_span<NonThrowingDefault>(3);
   const auto *expected_address = probe.data();
   allocator.reset();

   ThrowingDefault::constructions = 0;
   REQUIRE_THROWS_AS(allocator.allocate_span<ThrowingDefault>(3), std::runtime_error);

   const auto recovered = allocator.allocate_span<NonThrowingDefault>(3);
   REQUIRE(recovered.data() == expected_address);
}
