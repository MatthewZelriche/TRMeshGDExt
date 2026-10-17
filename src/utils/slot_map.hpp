#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Describes a SlotMap column. The value type is also its tag by default;
// provide a distinct tag when more than one column stores the same type.
template<typename T, typename TTag = T>
struct Col {
   using Type = T;
   using Tag = TTag;
};

namespace slot_map_detail {

template<typename T>
concept ValidColumn =
    requires {
       typename T::Type;
       typename T::Tag;
    } && std::same_as<T, Col<typename T::Type, typename T::Tag>> &&
    std::is_object_v<typename T::Type> && !std::is_const_v<typename T::Type> &&
    !std::is_volatile_v<typename T::Type> &&
    !std::same_as<typename T::Type, bool> && // because of std::vector<bool> nonsense
    std::destructible<typename T::Type> && std::is_nothrow_move_constructible_v<typename T::Type>;

template<typename Tag, typename... Columns>
concept HasTag = (std::same_as<Tag, typename Columns::Tag> || ...);

template<typename... Ts>
struct AreUnique : std::true_type {};

template<typename T, typename... Rest>
struct AreUnique<T, Rest...> :
    std::bool_constant<((!std::same_as<T, Rest>) && ...) && AreUnique<Rest...>::value> {};

} // namespace slot_map_detail

// A packed structure-of-arrays slot map with amortized O(1) insertion, O(1)
// removal and lookup, and contiguous storage for every column.
//
// Pointers, references, spans and iterators are to be considered invalidated
// after the following operations:
// - emplace()
// - insert()
// - remove()
// - reserve()
// - clear()
//
// Handles are intentionally not generation-counted. Consequently, a stale
// handle can refer to a new row after its sparse slot is reused.
template<typename... Columns>
requires(sizeof...(Columns) > 0) && (slot_map_detail::ValidColumn<Columns> && ...)
class SlotMap {
  public:
   using Handle = std::uint64_t;
   using SizeType = std::size_t;

   static constexpr SizeType COLUMN_COUNT = sizeof...(Columns);
   static constexpr Handle INVALID_HANDLE = std::numeric_limits<Handle>::max();

   template<SizeType Index>
   using ColumnType = std::tuple_element_t<Index, std::tuple<Columns...>>::Type;

   SlotMap() = default;

   explicit SlotMap(SizeType capacity) { reserve(capacity); }

   SlotMap(const SlotMap &) = delete;
   SlotMap &operator=(const SlotMap &) = delete;

   SlotMap(SlotMap &&other) noexcept :
       mDenseColumns(std::move(other.mDenseColumns)),
       mDenseToSparse(std::move(other.mDenseToSparse)),
       mSparseToDense(std::move(other.mSparseToDense)),
       mFreeHead(std::exchange(other.mFreeHead, FREE_LIST_END)) {
      other.clear();
   }

   SlotMap &operator=(SlotMap &&other) noexcept {
      if (this != &other) {
         mDenseColumns = std::move(other.mDenseColumns);
         mDenseToSparse = std::move(other.mDenseToSparse);
         mSparseToDense = std::move(other.mSparseToDense);
         mFreeHead = std::exchange(other.mFreeHead, FREE_LIST_END);
         other.clear();
      }
      return *this;
   }

   ~SlotMap() = default;

   [[nodiscard]] Handle emplace()
   requires(std::default_initializable<typename Columns::Type> && ...)
   {
      assert(mSparseToDense.size() < static_cast<SizeType>(FREE_LIST_END) ||
             mFreeHead != FREE_LIST_END);
      const bool reuse_sparse_slot = mFreeHead != FREE_LIST_END;
      append_default_row(ColumnIndices {});
      return finish_insert(reuse_sparse_slot);
   }

   template<typename... Values>
   requires(row_constructible<Values...>())
   [[nodiscard]] Handle emplace(Values &&...values) {
      assert(mSparseToDense.size() < static_cast<SizeType>(FREE_LIST_END) ||
             mFreeHead != FREE_LIST_END);
      const bool reuse_sparse_slot = mFreeHead != FREE_LIST_END;

      auto row_values = std::forward_as_tuple(std::forward<Values>(values)...);
      append_row(std::move(row_values), ColumnIndices {});
      return finish_insert(reuse_sparse_slot);
   }

   [[nodiscard]] Handle insert()
   requires(std::default_initializable<typename Columns::Type> && ...)
   {
      return emplace();
   }

   template<typename... Values>
   requires(row_constructible<Values...>())
   [[nodiscard]] Handle insert(Values &&...values) {
      return emplace(std::forward<Values>(values)...);
   }

   [[nodiscard]] bool remove(Handle handle) noexcept {
      const Handle sparse_entry = find_dense_index(handle);
      if (sparse_entry == INVALID_HANDLE) { return false; }

      const SizeType sparse_index = to_index(handle);
      const SizeType dense_index = to_index(sparse_entry);
      const SizeType last_dense_index = size() - 1;
      assert(dense_index <= last_dense_index);

      if (dense_index != last_dense_index) {
         relocate_row(dense_index, last_dense_index, ColumnIndices {});

         const Handle moved_handle = mDenseToSparse[last_dense_index];
         mDenseToSparse[dense_index] = moved_handle;
         mSparseToDense[to_index(moved_handle)] = static_cast<Handle>(dense_index);
      }

      pop_row(ColumnIndices {});
      mDenseToSparse.pop_back();
      mSparseToDense[sparse_index] = encode_free(mFreeHead);
      mFreeHead = handle;
      return true;
   }

   void reserve(SizeType capacity) {
      if (static_cast<Handle>(capacity) > FREE_LIST_END) {
         throw std::length_error("SlotMap capacity exceeds handle space");
      }

      reserve_dense_columns(capacity, ColumnIndices {});
      mDenseToSparse.reserve(capacity);
      mSparseToDense.reserve(capacity);
   }

   void clear() noexcept {
      clear_columns(ColumnIndices {});
      mDenseToSparse.clear();
      mSparseToDense.clear();
      mFreeHead = FREE_LIST_END;
   }

   [[nodiscard]] bool contains(Handle handle) const noexcept {
      return find_dense_index(handle) != INVALID_HANDLE;
   }

   template<typename Tag>
   requires slot_map_detail::HasTag<Tag, Columns...>
   [[nodiscard]] auto *get(Handle handle) noexcept {
      return get<tag_index<Tag>()>(handle);
   }

   template<typename Tag>
   requires slot_map_detail::HasTag<Tag, Columns...>
   [[nodiscard]] const auto *get(Handle handle) const noexcept {
      return get<tag_index<Tag>()>(handle);
   }

   [[nodiscard]] ColumnType<0> *get(Handle handle) noexcept
   requires(COLUMN_COUNT == 1)
   {
      return get<0>(handle);
   }

   [[nodiscard]] const ColumnType<0> *get(Handle handle) const noexcept
   requires(COLUMN_COUNT == 1)
   {
      return get<0>(handle);
   }

   template<SizeType Index>
   requires(Index < COLUMN_COUNT)
   [[nodiscard]] ColumnType<Index> *get(Handle handle) noexcept {
      const Handle dense_index = find_dense_index(handle);
      if (dense_index == INVALID_HANDLE) { return nullptr; }

      assert(dense_index < static_cast<Handle>(size()));
      return std::addressof(std::get<Index>(mDenseColumns)[to_index(dense_index)]);
   }

   template<SizeType Index>
   requires(Index < COLUMN_COUNT)
   [[nodiscard]] const ColumnType<Index> *get(Handle handle) const noexcept {
      const Handle dense_index = find_dense_index(handle);
      if (dense_index == INVALID_HANDLE) { return nullptr; }

      assert(dense_index < static_cast<Handle>(size()));
      return std::addressof(std::get<Index>(mDenseColumns)[to_index(dense_index)]);
   }

   [[nodiscard]] auto row(Handle handle) noexcept {
      const Handle dense_index = find_dense_index(handle);
      assert(dense_index != INVALID_HANDLE);
      return row_at(to_index(dense_index), ColumnIndices {});
   }

   [[nodiscard]] auto row(Handle handle) const noexcept {
      const Handle dense_index = find_dense_index(handle);
      assert(dense_index != INVALID_HANDLE);
      return row_at(to_index(dense_index), ColumnIndices {});
   }

   // Precondition: handle refers to an active slot. Available only on a
   // single-column SlotMap; use get<Index>() or row() for multiple columns.
   ColumnType<0> &operator[](Handle handle) noexcept
   requires(COLUMN_COUNT == 1)
   {
      assert(contains(handle));
      return *get<0>(handle);
   }

   const ColumnType<0> &operator[](Handle handle) const noexcept
   requires(COLUMN_COUNT == 1)
   {
      assert(contains(handle));
      return *get<0>(handle);
   }

   [[nodiscard]] bool empty() const noexcept { return std::get<0>(mDenseColumns).empty(); }

   [[nodiscard]] SizeType size() const noexcept { return std::get<0>(mDenseColumns).size(); }

   // Returns the capacity shared by every column (the smallest column capacity).
   [[nodiscard]] SizeType capacity() const noexcept {
      SizeType result = std::get<0>(mDenseColumns).capacity();
      std::apply(
          [&result](const auto &...columns) {
             ((result = result < columns.capacity() ? result : columns.capacity()), ...);
          },
          mDenseColumns);
      return result;
   }

   [[nodiscard]] SizeType slot_count() const noexcept { return mSparseToDense.size(); }

   [[nodiscard]] auto columns() noexcept { return column_spans(ColumnIndices {}); }

   [[nodiscard]] auto columns() const noexcept { return column_spans(ColumnIndices {}); }

   template<typename Tag>
   requires slot_map_detail::HasTag<Tag, Columns...>
   [[nodiscard]] auto column() noexcept {
      return column<tag_index<Tag>()>();
   }

   template<typename Tag>
   requires slot_map_detail::HasTag<Tag, Columns...>
   [[nodiscard]] auto column() const noexcept {
      return column<tag_index<Tag>()>();
   }

   [[nodiscard]] ColumnType<0> *data() noexcept
   requires(COLUMN_COUNT == 1)
   {
      return std::get<0>(mDenseColumns).data();
   }

   [[nodiscard]] const ColumnType<0> *data() const noexcept
   requires(COLUMN_COUNT == 1)
   {
      return std::get<0>(mDenseColumns).data();
   }

   [[nodiscard]] std::span<ColumnType<0>> values() noexcept
   requires(COLUMN_COUNT == 1)
   {
      return column<0>();
   }

   [[nodiscard]] std::span<const ColumnType<0>> values() const noexcept
   requires(COLUMN_COUNT == 1)
   {
      return column<0>();
   }

   template<SizeType Index>
   requires(Index < COLUMN_COUNT)
   [[nodiscard]] std::span<ColumnType<Index>> column() noexcept {
      return std::get<Index>(mDenseColumns);
   }

   template<SizeType Index>
   requires(Index < COLUMN_COUNT)
   [[nodiscard]] std::span<const ColumnType<Index>> column() const noexcept {
      return std::get<Index>(mDenseColumns);
   }

  private:
   using ColumnTuple = std::tuple<Columns...>;
   using ColumnIndices = std::make_index_sequence<COLUMN_COUNT>;

   static_assert(sizeof(Handle) == sizeof(std::uint64_t), "SlotMap handles must be 64 bits");
   static_assert(
       slot_map_detail::AreUnique<typename Columns::Tag...>::value,
       "SlotMap column tags must be unique; use Col<T, Tag> to distinguish duplicate types");

   // mSparseToDense entries with FREE_BIT set contain an embedded freelist
   // link in their low bits. Active entries contain an untagged dense index.
   static constexpr Handle FREE_BIT = Handle {1} << 63;
   static constexpr Handle INDEX_MASK = FREE_BIT - 1;
   static constexpr Handle FREE_LIST_END = INDEX_MASK;

   std::tuple<std::vector<typename Columns::Type>...> mDenseColumns;
   std::vector<Handle> mDenseToSparse;
   std::vector<Handle> mSparseToDense;
   Handle mFreeHead = FREE_LIST_END;

   [[nodiscard]] static constexpr bool is_free(Handle entry) noexcept {
      return (entry & FREE_BIT) != 0;
   }

   [[nodiscard]] static constexpr Handle encode_free(Handle next) noexcept {
      return FREE_BIT | next;
   }

   [[nodiscard]] static constexpr Handle decode_free(Handle entry) noexcept {
      return entry & INDEX_MASK;
   }

   [[nodiscard]] static constexpr SizeType to_index(Handle index) noexcept {
      return static_cast<SizeType>(index);
   }

   template<typename Tag, SizeType Index = 0>
   [[nodiscard]] static consteval SizeType tag_index() {
      if constexpr (Index == COLUMN_COUNT) {
         return COLUMN_COUNT;
      } else if constexpr (std::same_as<Tag,
                                        typename std::tuple_element_t<Index, ColumnTuple>::Tag>) {
         return Index;
      } else {
         return tag_index<Tag, Index + 1>();
      }
   }

   template<SizeType... Indices>
   void reserve_dense_columns(SizeType capacity, std::index_sequence<Indices...>) {
      (std::get<Indices>(mDenseColumns).reserve(capacity), ...);
   }

   template<typename Tuple, SizeType... Indices>
   void append_row(Tuple &&values, std::index_sequence<Indices...>) {
      SizeType appended = 0;

      // Try to insert values for all rows. Exceptions due to things such as constructor failure
      // maintain integrity of the data structure by rolling back changes.
      try {
         (
             [&] {
                std::get<Indices>(mDenseColumns)
                    .emplace_back(std::get<Indices>(std::forward<Tuple>(values)));
                ++appended;
             }(),
             ...);
      } catch (...) {
         (
             [&] {
                constexpr SizeType index = COLUMN_COUNT - 1 - Indices;
                if (index < appended) { std::get<index>(mDenseColumns).pop_back(); }
             }(),
             ...);
         throw;
      }
   }

   template<SizeType... Indices>
   void append_default_row(std::index_sequence<Indices...>) {
      SizeType appended = 0;

      // Try to insert values for all rows. Exceptions due to things such as constructor failure
      // maintain integrity of the data structure by rolling back changes.
      try {
         (
             [&] {
                std::get<Indices>(mDenseColumns).emplace_back();
                ++appended;
             }(),
             ...);
      } catch (...) {
         (
             [&] {
                constexpr SizeType index = COLUMN_COUNT - 1 - Indices;
                if (index < appended) { std::get<index>(mDenseColumns).pop_back(); }
             }(),
             ...);
         throw;
      }
   }

   template<typename Tuple, SizeType... Indices>
   [[nodiscard]] static consteval bool row_constructible(std::index_sequence<Indices...>) {
      return (std::constructible_from<ColumnType<Indices>,
                                      decltype(std::get<Indices>(std::declval<Tuple &&>()))> &&
              ...);
   }

   // Ensures at compile time that the types of provided values to insert and emplace
   // can actually be used to construct that row. Simply for producing cleaner compile errors
   template<typename... Values>
   [[nodiscard]] static consteval bool row_constructible() {
      if constexpr (sizeof...(Values) != COLUMN_COUNT) {
         return false;
      } else {
         return row_constructible<std::tuple<Values &&...>>(ColumnIndices {});
      }
   }

   // Updates the book-keeping information after inserting an entire row of dense values
   [[nodiscard]] Handle finish_insert(bool reuse_sparse_slot) noexcept {
      const auto dense_index = static_cast<Handle>(size() - 1);
      Handle handle = INVALID_HANDLE;

      if (reuse_sparse_slot) {
         handle = mFreeHead;

         const Handle free_entry = mSparseToDense[to_index(handle)];
         assert(is_free(free_entry));

         mFreeHead = decode_free(free_entry);
         mSparseToDense[to_index(handle)] = dense_index;
      } else {
         handle = static_cast<Handle>(mSparseToDense.size());
         mSparseToDense.push_back(dense_index);
      }

      mDenseToSparse.push_back(handle);
      return handle;
   }

   // Moves source into destination. Similar to the first half of a swap & pop,
   // but we save a small amount of time by not bothering to move destination back to source
   // (it will just be popped right after anyway)
   template<SizeType Index>
   void relocate_column(SizeType destination, SizeType source) noexcept {
      auto &column = std::get<Index>(mDenseColumns);
      using T = ColumnType<Index>;

      if constexpr (std::is_nothrow_move_assignable_v<T>) {
         column[destination] = std::move(column[source]);
      } else {
         std::destroy_at(std::addressof(column[destination]));
         std::construct_at(std::addressof(column[destination]), std::move(column[source]));
      }
   }

   // Relocates all the column entries associated with a handle
   template<SizeType... Indices>
   void relocate_row(SizeType destination, SizeType source,
                     std::index_sequence<Indices...>) noexcept {
      (relocate_column<Indices>(destination, source), ...);
   }

   template<SizeType... Indices>
   void pop_row(std::index_sequence<Indices...>) noexcept {
      (std::get<Indices>(mDenseColumns).pop_back(), ...);
   }

   template<SizeType... Indices>
   void clear_columns(std::index_sequence<Indices...>) noexcept {
      (std::get<Indices>(mDenseColumns).clear(), ...);
   }

   [[nodiscard]] Handle find_dense_index(Handle handle) const noexcept {
      if (handle >= static_cast<Handle>(mSparseToDense.size())) { return INVALID_HANDLE; }

      const Handle sparse_entry = mSparseToDense[to_index(handle)];
      return is_free(sparse_entry) ? INVALID_HANDLE : sparse_entry;
   }

   template<SizeType... Indices>
   [[nodiscard]] auto row_at(SizeType dense_index, std::index_sequence<Indices...>) noexcept {
      return std::forward_as_tuple(std::get<Indices>(mDenseColumns)[dense_index]...);
   }

   template<SizeType... Indices>
   [[nodiscard]] auto row_at(SizeType dense_index, std::index_sequence<Indices...>) const noexcept {
      return std::forward_as_tuple(std::get<Indices>(mDenseColumns)[dense_index]...);
   }

   template<SizeType... Indices>
   [[nodiscard]] auto column_spans(std::index_sequence<Indices...>) noexcept {
      return std::tuple {std::span<ColumnType<Indices>>(std::get<Indices>(mDenseColumns))...};
   }

   template<SizeType... Indices>
   [[nodiscard]] auto column_spans(std::index_sequence<Indices...>) const noexcept {
      return std::tuple {std::span<const ColumnType<Indices>>(std::get<Indices>(mDenseColumns))...};
   }
};
