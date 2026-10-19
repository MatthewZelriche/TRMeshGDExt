#pragma once

#include <cassert>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
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

// Enforces at compile-time that records are applied only to types that match
// this stronger required invariant for type recording
template<typename... Columns>
concept RecordableColumns = (std::is_nothrow_copy_constructible_v<typename Columns::Type> && ...) &&
                            (std::is_nothrow_copy_assignable_v<typename Columns::Type> && ...) &&
                            (std::default_initializable<typename Columns::Type> && ...);

template<typename... Ts>
struct AreUnique : std::true_type {};

template<typename T, typename... Rest>
struct AreUnique<T, Rest...> :
    std::bool_constant<((!std::same_as<T, Rest>) && ...) && AreUnique<Rest...>::value> {};

} // namespace slot_map_detail

template<typename MapTag>
class SlotHandle {
  public:
   using ValueType = std::uint64_t;

   static constexpr ValueType INVALID_VALUE = std::numeric_limits<ValueType>::max();

   constexpr SlotHandle() noexcept = default;

   explicit constexpr SlotHandle(ValueType value) noexcept : mValue(value) {}

   constexpr ValueType val() const noexcept { return mValue; }

   explicit constexpr operator ValueType() const noexcept { return mValue; }

   constexpr auto operator<=>(const SlotHandle &) const noexcept = default;

  private:
   ValueType mValue = INVALID_VALUE;
};

// A packed structure-of-arrays slot map with amortized O(1) insertion, O(1)
// removal and lookup, and contiguous storage for every column.
// MapTag provides the map's identity and must precede one or more Col types.
// Each distinct MapTag produces a distinct Handle type.
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
template<typename MapTag, typename... Columns>
requires(sizeof...(Columns) > 0) && (slot_map_detail::ValidColumn<Columns> && ...)
class SlotMap {
  public:
   using Handle = SlotHandle<MapTag>;
   using SizeType = std::size_t;

   static constexpr SizeType COLUMN_COUNT = sizeof...(Columns);
   static constexpr Handle INVALID_HANDLE {Handle::INVALID_VALUE};

   template<SizeType Index>
   using ColumnType = std::tuple_element_t<Index, std::tuple<Columns...>>::Type;

   SlotMap() = default;

   explicit SlotMap(SizeType capacity) { reserve(capacity); }

   SlotMap(const SlotMap &) = delete;
   SlotMap &operator=(const SlotMap &) = delete;

   SlotMap(SlotMap &&other) noexcept {
      // TODO: not a fan of std::terminate() here and in the move assignment
      // & destructor.
      if (other.mIsRecording) { std::terminate(); }
      mDenseColumns = std::move(other.mDenseColumns);
      mDenseToSparse = std::move(other.mDenseToSparse);
      mSparseToDense = std::move(other.mSparseToDense);
      mFreeHead = std::exchange(other.mFreeHead, FREE_LIST_END);
      other.clear_storage();
   }

   SlotMap &operator=(SlotMap &&other) noexcept {
      if (mIsRecording || other.mIsRecording) { std::terminate(); }
      if (this != &other) {
         mDenseColumns = std::move(other.mDenseColumns);
         mDenseToSparse = std::move(other.mDenseToSparse);
         mSparseToDense = std::move(other.mSparseToDense);
         mFreeHead = std::exchange(other.mFreeHead, FREE_LIST_END);
         other.clear_storage();
      }
      return *this;
   }

   ~SlotMap() {
      if (mIsRecording) { std::terminate(); }
   }

   [[nodiscard]] Handle emplace()
   requires(std::default_initializable<typename Columns::Type> && ...)
   {
      assert(mSparseToDense.size() < static_cast<SizeType>(FREE_LIST_END) ||
             mFreeHead != FREE_LIST_END);
      const SizeType previous_slot_count = slot_count();
      const bool reuse_sparse_slot = mFreeHead != FREE_LIST_END;
      append_default_row(ColumnIndices {});
      const Handle handle = finish_insert(reuse_sparse_slot);
      record_insert(handle, slot_count() != previous_slot_count);
      return handle;
   }

   template<typename... Values>
   requires(row_constructible<Values...>())
   [[nodiscard]] Handle emplace(Values &&...values) {
      assert(mSparseToDense.size() < static_cast<SizeType>(FREE_LIST_END) ||
             mFreeHead != FREE_LIST_END);
      const SizeType previous_slot_count = slot_count();
      const bool reuse_sparse_slot = mFreeHead != FREE_LIST_END;

      auto row_values = std::forward_as_tuple(std::forward<Values>(values)...);
      append_row(std::move(row_values), ColumnIndices {});
      const Handle handle = finish_insert(reuse_sparse_slot);
      record_insert(handle, slot_count() != previous_slot_count);
      return handle;
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

   [[nodiscard]] bool remove(Handle handle) {
      const HandleValue sparse_entry = find_dense_index(handle);
      if (sparse_entry == Handle::INVALID_VALUE) { return false; }

      capture(handle);
      const HandleValue handle_value = to_value(handle);
      const SizeType sparse_index = to_index(handle_value);
      const SizeType dense_index = to_index(sparse_entry);
      const SizeType last_dense_index = size() - 1;
      assert(dense_index <= last_dense_index);

      if (dense_index != last_dense_index) {
         relocate_row(dense_index, last_dense_index, ColumnIndices {});

         const HandleValue moved_handle = mDenseToSparse[last_dense_index];
         mDenseToSparse[dense_index] = moved_handle;
         mSparseToDense[to_index(moved_handle)] = static_cast<HandleValue>(dense_index);
      }

      pop_row(ColumnIndices {});
      mDenseToSparse.pop_back();
      mSparseToDense[sparse_index] = encode_free(mFreeHead);
      mFreeHead = handle_value;
      record_remove(dense_index);
      return true;
   }

   void reserve(SizeType capacity) {
      if (static_cast<HandleValue>(capacity) > FREE_LIST_END) {
         throw std::length_error("SlotMap capacity exceeds handle space");
      }

      reserve_dense_columns(capacity, ColumnIndices {});
      mDenseToSparse.reserve(capacity);
      mSparseToDense.reserve(capacity);
   }

   // Ensures room for the requested number of rows, growing geometrically to
   // avoid repeated exact-size reservations
   void ensure_capacity(SizeType required) {
      const SizeType current = capacity();
      if (required <= current) { return; }

      const auto maximum = static_cast<SizeType>(FREE_LIST_END);
      if (required > maximum) { throw std::length_error("SlotMap capacity exceeds handle space"); }

      const SizeType half = current / 2;
      const SizeType geometric = current > maximum - half ? maximum : current + half;
      reserve(std::max(required, geometric));
   }

   void clear() {
      if (mIsRecording) {
         throw std::logic_error("clear is not supported while the slot map is recording");
      }
      clear_storage();
   }

   bool contains(Handle handle) const noexcept {
      return find_dense_index(handle) != Handle::INVALID_VALUE;
   }

   template<typename Tag>
   requires slot_map_detail::HasTag<Tag, Columns...>
   const auto *get(Handle handle) const noexcept {
      return get<tag_index<Tag>()>(handle);
   }

   const ColumnType<0> *get(Handle handle) const noexcept
   requires(COLUMN_COUNT == 1)
   {
      return get<0>(handle);
   }

   template<SizeType Index>
   requires(Index < COLUMN_COUNT)
   const ColumnType<Index> *get(Handle handle) const noexcept {
      const HandleValue dense_index = find_dense_index(handle);
      if (dense_index == Handle::INVALID_VALUE) { return nullptr; }

      assert(dense_index < static_cast<HandleValue>(size()));
      return std::addressof(std::get<Index>(mDenseColumns)[to_index(dense_index)]);
   }

   template<typename Tag>
   requires slot_map_detail::HasTag<Tag, Columns...>
   auto *get_mut(Handle handle) {
      return get_mut<tag_index<Tag>()>(handle);
   }

   template<SizeType Index>
   requires(Index < COLUMN_COUNT)
   ColumnType<Index> *get_mut(Handle handle) {
      const HandleValue dense_index = find_dense_index(handle);
      if (dense_index == Handle::INVALID_VALUE) { return nullptr; }

      capture(handle);
      assert(dense_index < static_cast<HandleValue>(size()));
      return std::addressof(std::get<Index>(mDenseColumns)[to_index(dense_index)]);
   }

   ColumnType<0> *get_mut(Handle handle)
   requires(COLUMN_COUNT == 1)
   {
      return get_mut<0>(handle);
   }

   auto row(Handle handle) const noexcept {
      const HandleValue dense_index = find_dense_index(handle);
      assert(dense_index != Handle::INVALID_VALUE);
      return row_at(to_index(dense_index), ColumnIndices {});
   }

   auto row_mut(Handle handle) {
      const HandleValue dense_index = find_dense_index(handle);
      assert(dense_index != Handle::INVALID_VALUE);
      capture(handle);
      return row_at(to_index(dense_index), ColumnIndices {});
   }

   // Precondition: handle refers to an active slot. Available only on a
   // single-column SlotMap; use get()/get_mut() or row()/row_mut() for multiple columns.
   ColumnType<0> &operator[](Handle handle)
   requires(COLUMN_COUNT == 1)
   {
      assert(contains(handle));
      return *get_mut<0>(handle);
   }

   const ColumnType<0> &operator[](Handle handle) const noexcept
   requires(COLUMN_COUNT == 1)
   {
      assert(contains(handle));
      return *get<0>(handle);
   }

   bool empty() const noexcept { return std::get<0>(mDenseColumns).empty(); }

   SizeType size() const noexcept { return std::get<0>(mDenseColumns).size(); }

   // Returns the row capacity shared by every backing vector.
   SizeType capacity() const noexcept {
      SizeType result = std::get<0>(mDenseColumns).capacity();
      std::apply(
          [&result](const auto &...columns) {
             ((result = result < columns.capacity() ? result : columns.capacity()), ...);
          },
          mDenseColumns);
      result = std::min(result, mDenseToSparse.capacity());
      result = std::min(result, mSparseToDense.capacity());
      return result;
   }

   SizeType slot_count() const noexcept { return mSparseToDense.size(); }

   Handle handle_at(SizeType dense_index) const noexcept {
      assert(dense_index < size());
      return Handle {mDenseToSparse[dense_index]};
   }

   auto columns() const noexcept { return column_spans(ColumnIndices {}); }

   auto columns_mut() {
      capture_all();
      return column_spans(ColumnIndices {});
   }

   template<typename Tag>
   requires slot_map_detail::HasTag<Tag, Columns...>
   auto column() const noexcept {
      return column<tag_index<Tag>()>();
   }

   template<typename Tag>
   requires slot_map_detail::HasTag<Tag, Columns...>
   auto column_mut() {
      return column_mut<tag_index<Tag>()>();
   }

   const ColumnType<0> *data() const noexcept
   requires(COLUMN_COUNT == 1)
   {
      return std::get<0>(mDenseColumns).data();
   }

   ColumnType<0> *data_mut()
   requires(COLUMN_COUNT == 1)
   {
      capture_all();
      return std::get<0>(mDenseColumns).data();
   }

   std::span<const ColumnType<0>> values() const noexcept
   requires(COLUMN_COUNT == 1)
   {
      return column<0>();
   }

   std::span<ColumnType<0>> values_mut()
   requires(COLUMN_COUNT == 1)
   {
      return column_mut<0>();
   }

   template<SizeType Index>
   requires(Index < COLUMN_COUNT)
   std::span<const ColumnType<Index>> column() const noexcept {
      return std::get<Index>(mDenseColumns);
   }

   template<SizeType Index>
   requires(Index < COLUMN_COUNT)
   std::span<ColumnType<Index>> column_mut() {
      capture_all();
      return std::get<Index>(mDenseColumns);
   }

  private:
   using HandleValue = std::uint64_t;
   using ColumnTuple = std::tuple<Columns...>;
   using ColumnIndices = std::make_index_sequence<COLUMN_COUNT>;
   using StoredRow = std::tuple<typename Columns::Type...>;

   enum class EditKind : std::uint8_t { InsertRun, Remove };

   struct StructuralEdit {
      // Stores either dense index or run-length encoding of consecutive compatible insertions
      SizeType value;
      EditKind kind;
      bool grew;
   };

   struct SavedRow {
      Handle handle;
      StoredRow row;
   };

   static_assert(sizeof(Handle) == sizeof(std::uint64_t), "SlotMap handles must be 64 bits");
   static_assert(
       slot_map_detail::AreUnique<typename Columns::Tag...>::value,
       "SlotMap column tags must be unique; use Col<T, Tag> to distinguish duplicate types");

   // mSparseToDense entries with FREE_BIT set contain an embedded freelist
   // link in their low bits. Active entries contain an untagged dense index.
   static constexpr HandleValue FREE_BIT = HandleValue {1} << 63;
   static constexpr HandleValue INDEX_MASK = FREE_BIT - 1;
   static constexpr HandleValue FREE_LIST_END = INDEX_MASK;

   std::tuple<std::vector<typename Columns::Type>...> mDenseColumns;
   std::vector<HandleValue> mDenseToSparse;
   std::vector<HandleValue> mSparseToDense;
   HandleValue mFreeHead = FREE_LIST_END;
   std::vector<StructuralEdit> mStructure;
   std::vector<SavedRow> mSavedRows;
   std::vector<bool> mTouched;
   bool mIsRecording = false;

   static constexpr bool is_free(HandleValue entry) noexcept { return (entry & FREE_BIT) != 0; }

   static constexpr HandleValue encode_free(HandleValue next) noexcept { return FREE_BIT | next; }

   static constexpr HandleValue decode_free(HandleValue entry) noexcept {
      return entry & INDEX_MASK;
   }

   static constexpr HandleValue to_value(Handle handle) noexcept {
      return static_cast<HandleValue>(handle);
   }

   static constexpr SizeType to_index(HandleValue index) noexcept {
      return static_cast<SizeType>(index);
   }

   template<typename Tag, SizeType Index = 0>
   static consteval SizeType tag_index() {
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
   StoredRow copy_row(SizeType dense_index, std::index_sequence<Indices...>) const {
      return StoredRow {std::get<Indices>(mDenseColumns)[dense_index]...};
   }

   StoredRow copy_row(Handle handle) const {
      const HandleValue dense_index = find_dense_index(handle);
      assert(dense_index != Handle::INVALID_VALUE);
      return copy_row(to_index(dense_index), ColumnIndices {});
   }

   template<SizeType... Indices>
   void assign_row(SizeType dense_index, const StoredRow &row,
                   std::index_sequence<Indices...>) noexcept {
      ((std::get<Indices>(mDenseColumns)[dense_index] = std::get<Indices>(row)), ...);
   }

   void assign_row(Handle handle, const StoredRow &row) noexcept {
      const HandleValue dense_index = find_dense_index(handle);
      assert(dense_index != Handle::INVALID_VALUE);
      assign_row(to_index(dense_index), row, ColumnIndices {});
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
   static consteval bool row_constructible(std::index_sequence<Indices...>) {
      return (std::constructible_from<ColumnType<Indices>,
                                      decltype(std::get<Indices>(std::declval<Tuple &&>()))> &&
              ...);
   }

   // Ensures at compile time that the types of provided values to insert and emplace
   // can actually be used to construct that row. Simply for producing cleaner compile errors
   template<typename... Values>
   static consteval bool row_constructible() {
      if constexpr (sizeof...(Values) != COLUMN_COUNT) {
         return false;
      } else {
         return row_constructible<std::tuple<Values &&...>>(ColumnIndices {});
      }
   }

   // Updates the book-keeping information after inserting an entire row of dense values
   [[nodiscard]] Handle finish_insert(bool reuse_sparse_slot) noexcept {
      const auto dense_index = static_cast<HandleValue>(size() - 1);
      HandleValue handle = Handle::INVALID_VALUE;

      if (reuse_sparse_slot) {
         handle = mFreeHead;

         const HandleValue free_entry = mSparseToDense[to_index(handle)];
         assert(is_free(free_entry));

         mFreeHead = decode_free(free_entry);
         mSparseToDense[to_index(handle)] = dense_index;
      } else {
         handle = static_cast<HandleValue>(mSparseToDense.size());
         mSparseToDense.push_back(dense_index);
      }

      mDenseToSparse.push_back(handle);
      return Handle {handle};
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

   HandleValue find_dense_index(Handle handle) const noexcept {
      const HandleValue handle_value = to_value(handle);
      if (handle_value >= static_cast<HandleValue>(mSparseToDense.size())) {
         return Handle::INVALID_VALUE;
      }

      const HandleValue sparse_entry = mSparseToDense[to_index(handle_value)];
      return is_free(sparse_entry) ? Handle::INVALID_VALUE : sparse_entry;
   }

   template<SizeType... Indices>
   auto row_at(SizeType dense_index, std::index_sequence<Indices...>) noexcept {
      return std::forward_as_tuple(std::get<Indices>(mDenseColumns)[dense_index]...);
   }

   template<SizeType... Indices>
   auto row_at(SizeType dense_index, std::index_sequence<Indices...>) const noexcept {
      return std::forward_as_tuple(std::get<Indices>(mDenseColumns)[dense_index]...);
   }

   template<SizeType... Indices>
   auto column_spans(std::index_sequence<Indices...>) noexcept {
      return std::tuple {std::span<ColumnType<Indices>>(std::get<Indices>(mDenseColumns))...};
   }

   template<SizeType... Indices>
   auto column_spans(std::index_sequence<Indices...>) const noexcept {
      return std::tuple {std::span<const ColumnType<Indices>>(std::get<Indices>(mDenseColumns))...};
   }

   void clear_storage() noexcept {
      assert(!mIsRecording);
      clear_columns(ColumnIndices {});
      mDenseToSparse.clear();
      mSparseToDense.clear();
      mFreeHead = FREE_LIST_END;
   }

  public:
   // ********************************************
   // Begin API For Recording Functionality
   // ********************************************
   class Patch {
     public:
      Patch() = default;
      ~Patch() = default;
      Patch(const Patch &) = delete;
      Patch &operator=(const Patch &) = delete;
      Patch(Patch &&) noexcept = default;
      Patch &operator=(Patch &&) noexcept = default;

      [[nodiscard]] std::size_t byte_size() const noexcept {
         return (mStructure.size() * sizeof(StructuralEdit)) +
                (mSavedRows.size() * sizeof(SavedRow));
      }

     private:
      friend class SlotMap;

      Patch(std::vector<StructuralEdit> structure, std::vector<SavedRow> saved_rows) noexcept :
          mStructure(std::move(structure)), mSavedRows(std::move(saved_rows)) {}

      std::vector<StructuralEdit> mStructure;
      std::vector<SavedRow> mSavedRows;
   };

   void begin_recording()
   requires slot_map_detail::RecordableColumns<Columns...>
   {
      if (mIsRecording) { throw std::logic_error("the slot map is already recording"); }

      mTouched.assign(slot_count(), false);
      mStructure.clear();
      mSavedRows.clear();
      mIsRecording = true;
   }

   [[nodiscard]] Patch commit_recording()
   requires slot_map_detail::RecordableColumns<Columns...>
   {
      if (!mIsRecording) { throw std::logic_error("the slot map is not recording"); }

      Patch patch {std::move(mStructure), std::move(mSavedRows)};
      clear_recording_state();
      return patch;
   }

   void rollback_recording() noexcept
   requires slot_map_detail::RecordableColumns<Columns...>
   {
      assert(mIsRecording);
      Patch patch {std::move(mStructure), std::move(mSavedRows)};
      clear_recording_state();
      revert(patch);
   }

   void cancel_recording() noexcept
   requires slot_map_detail::RecordableColumns<Columns...>
   {
      assert(mIsRecording);
      assert(mStructure.empty() && mSavedRows.empty());
      clear_recording_state();
   }

   void revert(const Patch &patch) noexcept
   requires slot_map_detail::RecordableColumns<Columns...>
   {
      assert(!mIsRecording);
      for (auto edit = patch.mStructure.rbegin(); edit != patch.mStructure.rend(); ++edit) {
         if (edit->kind == EditKind::Remove) {
            undo_remove(edit->value);
            continue;
         }

         for (SizeType remaining = edit->value; remaining > 0; --remaining) {
            undo_insert(edit->grew);
         }
      }

      for (const SavedRow &saved : patch.mSavedRows) { assign_row(saved.handle, saved.row); }
   }

   [[nodiscard]] bool is_recording() const noexcept { return mIsRecording; }

  private:
   // We operate on the assumption that allocation failures essentially never happen
   // and give methods that can throw such errors an explicit exemption here.
   // If our assumption is ever invalidated, we will still std::terminate()
   void record_insert(Handle handle, bool grew) noexcept {
      if constexpr (slot_map_detail::RecordableColumns<Columns...>) {
         if (!mIsRecording) { return; }

         const SizeType index = handle.val();

         if (grew) {
            assert(index == mTouched.size());
            mTouched.push_back(true);
         } else {
            assert(index < mTouched.size());
            mTouched[index] = true;
         }

         if (!mStructure.empty()) {
            StructuralEdit &last = mStructure.back();
            if (last.kind == EditKind::InsertRun && last.grew == grew) {
               ++last.value;
               return;
            }
         }

         mStructure.push_back({
             .kind = EditKind::InsertRun,
             .value = 1,
             .grew = grew,
         });
      }
   }

   void record_remove(SizeType dense_index) noexcept {
      if (!mIsRecording) { return; }
      mStructure.push_back({.kind = EditKind::Remove, .value = dense_index, .grew = false});
   }

   void capture(Handle handle) {
      if constexpr (slot_map_detail::RecordableColumns<Columns...>) {
         if (!mIsRecording) { return; }

         const SizeType index = handle.val();
         assert(index < mTouched.size());
         if (mTouched[index]) { return; }

         mSavedRows.push_back({.handle = handle, .row = copy_row(handle)});
         mTouched[index] = true;
      }
   }

   void capture_all() {
      if constexpr (slot_map_detail::RecordableColumns<Columns...>) {
         if (!mIsRecording) { return; }
         for (HandleValue handle : mDenseToSparse) { capture(Handle {handle}); }
      }
   }

   // Exactly reverses the latest insertion. No later structural edit may remain applied.
   void undo_insert(bool grew) noexcept {
      assert(!empty());
      const HandleValue handle = mDenseToSparse.back();
      pop_row(ColumnIndices {});
      mDenseToSparse.pop_back();

      if (grew) {
         assert(handle == mSparseToDense.size() - 1);
         mSparseToDense.pop_back();
      } else {
         mSparseToDense[to_index(handle)] = encode_free(mFreeHead);
         mFreeHead = handle;
      }
   }

   // Exactly reverses the latest removal and restores the vacated dense position.
   // Saved row values are restored after all structural edits have been reversed.
   void undo_remove(SizeType dense_index) noexcept
   requires(std::default_initializable<typename Columns::Type> && ...)
   {
      assert(mFreeHead != FREE_LIST_END);
      const HandleValue handle = mFreeHead;
      const HandleValue free_entry = mSparseToDense[to_index(handle)];
      assert(is_free(free_entry));
      mFreeHead = decode_free(free_entry);

      const SizeType dense_count = size();
      assert(dense_index <= dense_count);
      if (dense_index == dense_count) {
         append_default_row(ColumnIndices {});
         mDenseToSparse.push_back(handle);
      } else {
         StoredRow moved = copy_row(dense_index, ColumnIndices {});
         append_row(std::move(moved), ColumnIndices {});

         const HandleValue moved_handle = mDenseToSparse[dense_index];
         mDenseToSparse.push_back(moved_handle);
         mSparseToDense[to_index(moved_handle)] = static_cast<HandleValue>(dense_count);

         const StoredRow placeholder {};
         assign_row(dense_index, placeholder, ColumnIndices {});
         mDenseToSparse[dense_index] = handle;
      }

      mSparseToDense[to_index(handle)] = static_cast<HandleValue>(dense_index);
   }

   void clear_recording_state() noexcept {
      mStructure.clear();
      mSavedRows.clear();
      mTouched.clear();
      mIsRecording = false;
   }
};
