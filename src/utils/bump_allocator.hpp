#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

// An allocation arena made from stable, geometrically grown blocks.
// Individual allocations cannot be freed. Scoped checkpoints partially rewind
// the arena, while reset() invalidates every allocation and retains its blocks
// for reuse.
//
// Objects and spans are value-initialized before being returned. Their element
// types must be trivially destructible because rewinding does not invoke
// destructors.
class BumpAllocator {
  public:
   class Checkpoint {
     public:
      Checkpoint(const Checkpoint &) = delete;
      Checkpoint &operator=(const Checkpoint &) = delete;
      Checkpoint(Checkpoint &&other) noexcept;
      Checkpoint &operator=(Checkpoint &&other) = delete;
      ~Checkpoint() noexcept;

      // Keeps allocations made since this checkpoint instead of restoring it.
      void dismiss() noexcept { mAllocator = nullptr; }

     private:
      friend class BumpAllocator;

      Checkpoint(BumpAllocator &allocator, std::size_t block, std::size_t offset,
                 std::size_t generation) noexcept :
          mAllocator(&allocator), mBlock(block), mOffset(offset), mGeneration(generation) {}

      void restore() noexcept;

      BumpAllocator *mAllocator = nullptr;
      std::size_t mBlock;
      std::size_t mOffset;
      std::size_t mGeneration;
   };

   static constexpr std::size_t MIN_BLOCK_BYTES = 4096;

   explicit BumpAllocator(std::size_t minimum_block_bytes = MIN_BLOCK_BYTES) noexcept :
       mMinimumBlockBytes(minimum_block_bytes) {
      assert(minimum_block_bytes > 0);
   }

   ~BumpAllocator() = default;
   BumpAllocator(const BumpAllocator &) = delete;
   BumpAllocator &operator=(const BumpAllocator &) = delete;

   BumpAllocator(BumpAllocator &&other) noexcept :
       mBlocks(std::move(other.mBlocks)), mMinimumBlockBytes(other.mMinimumBlockBytes),
       mCurrentBlock(std::exchange(other.mCurrentBlock, 0)) {
      other.mBlocks.clear();
      other.invalidate_checkpoints();
   }

   BumpAllocator &operator=(BumpAllocator &&other) noexcept {
      if (this != &other) {
         invalidate_checkpoints();
         other.invalidate_checkpoints();
         mBlocks = std::move(other.mBlocks);
         mMinimumBlockBytes = other.mMinimumBlockBytes;
         mCurrentBlock = std::exchange(other.mCurrentBlock, 0);
         other.mBlocks.clear();
      }
      return *this;
   }

   [[nodiscard]] void *allocate_bytes(std::size_t size,
                                      std::size_t alignment = alignof(std::max_align_t)) {
      if (size == 0) { return nullptr; }

      assert(alignment > 0 && (alignment & (alignment - 1)) == 0 &&
             "alignment must be a nonzero power of two");

      if (void *allocation = try_existing_blocks(size, alignment)) { return allocation; }

      const std::size_t required_capacity = allocation_capacity(size, alignment);
      const std::size_t block_capacity = grown_block_capacity(required_capacity);
      mBlocks.emplace_back(block_capacity);
      mCurrentBlock = mBlocks.size();

      void *allocation = try_allocate(mBlocks.back(), size, alignment);
      assert(allocation != nullptr);
      return allocation;
   }

   template<typename T>
   requires std::default_initializable<T> && std::is_trivially_destructible_v<T>
   [[nodiscard]] T *create() {
      return allocate_span<T>(1).data();
   }

   template<typename T>
   requires std::default_initializable<T> && std::is_trivially_destructible_v<T>
   [[nodiscard]] std::span<T> allocate_span(std::size_t count) {
      if (count == 0) { return {}; }

      if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
         throw std::bad_array_new_length();
      }

      // Checkpoint to restore in the case of constructor throwing
      Checkpoint saved = checkpoint();
      void *storage = allocate_bytes(count * sizeof(T), alignof(T));
      T *elements = static_cast<T *>(storage);
      std::uninitialized_value_construct_n(elements, count);
      saved.dismiss();
      return {elements, count};
   }

   // Automatically restores this allocation position when the guard leaves
   // scope. Nested checkpoints must be destroyed in LIFO order. A checkpoint
   // must not outlive its allocator. Moving, resetting, or releasing the
   // allocator invalidates its active checkpoints, whose destructors then do
   // nothing.
   Checkpoint checkpoint() noexcept {
      if (mCurrentBlock == 0) { return {*this, 0, 0, mCheckpointGeneration}; }

      assert(mCurrentBlock <= mBlocks.size());
      return {*this, mCurrentBlock, mBlocks[mCurrentBlock - 1].offset, mCheckpointGeneration};
   }

   void reset() noexcept {
      invalidate_checkpoints();
      for (Block &block : mBlocks) { block.offset = 0; }
      mCurrentBlock = 0;
   }

   void release() noexcept {
      invalidate_checkpoints();
      std::vector<Block>().swap(mBlocks);
      mCurrentBlock = 0;
   }

  private:
   struct Block {
      // NOLINTBEGIN (modernize-avoid-c-arrays)
      explicit Block(std::size_t block_capacity) :
          storage(std::make_unique_for_overwrite<std::byte[]>(block_capacity)),
          capacity(block_capacity) {}

      std::unique_ptr<std::byte[]> storage;
      // NOLINTEND
      std::size_t capacity;
      std::size_t offset = 0;
   };

   [[nodiscard]] void *try_existing_blocks(std::size_t size, std::size_t alignment) {
      std::size_t block = mCurrentBlock == 0 ? 1 : mCurrentBlock;

      while (block <= mBlocks.size()) {
         if (void *allocation = try_allocate(mBlocks[block - 1], size, alignment)) {
            mCurrentBlock = block;
            return allocation;
         }
         ++block;
      }

      return nullptr;
   }

   [[nodiscard]] static void *try_allocate(Block &block, std::size_t size,
                                           std::size_t alignment) noexcept {
      assert(block.offset <= block.capacity);
      void *candidate = block.storage.get() + block.offset;
      std::size_t available = block.capacity - block.offset;

      if (std::align(alignment, size, candidate, available) == nullptr) { return nullptr; }

      const std::size_t aligned_offset = block.capacity - available;
      assert(aligned_offset <= block.capacity - size);
      block.offset = aligned_offset + size;
      return candidate;
   }

   static std::size_t allocation_capacity(std::size_t size, std::size_t alignment) {
      const std::size_t padding = alignment - 1;
      if (size > std::numeric_limits<std::size_t>::max() - padding) {
         throw std::bad_array_new_length();
      }
      return size + padding;
   }

   std::size_t grown_block_capacity(std::size_t required) const noexcept {
      if (mBlocks.empty()) { return std::max(required, mMinimumBlockBytes); }

      const std::size_t previous = mBlocks.back().capacity;
      if (previous > std::numeric_limits<std::size_t>::max() / 2) { return required; }

      return std::max(required, previous * 2);
   }

   std::size_t current_offset() const noexcept {
      if (mCurrentBlock == 0) { return 0; }

      assert(mCurrentBlock <= mBlocks.size());
      return mBlocks[mCurrentBlock - 1].offset;
   }

   void restore_to(std::size_t block, std::size_t offset) noexcept {
      assert(block <= mBlocks.size());
      assert(block < mCurrentBlock || (block == mCurrentBlock && offset <= current_offset()));

      if (block > 0) { mBlocks[block - 1].offset = offset; }

      for (std::size_t index = block; index < mBlocks.size(); ++index) {
         mBlocks[index].offset = 0;
      }
      mCurrentBlock = block;
   }

   void invalidate_checkpoints() noexcept { ++mCheckpointGeneration; }

   std::vector<Block> mBlocks;
   std::size_t mMinimumBlockBytes;
   std::size_t mCurrentBlock = 0;
   std::size_t mCheckpointGeneration = 0;
};

inline BumpAllocator::Checkpoint::Checkpoint(Checkpoint &&other) noexcept :
    mAllocator(std::exchange(other.mAllocator, nullptr)), mBlock(other.mBlock),
    mOffset(other.mOffset), mGeneration(other.mGeneration) {}

inline BumpAllocator::Checkpoint::~Checkpoint() noexcept { restore(); }

inline void BumpAllocator::Checkpoint::restore() noexcept {
   BumpAllocator *allocator = std::exchange(mAllocator, nullptr);
   if (allocator == nullptr || allocator->mCheckpointGeneration != mGeneration) { return; }
   allocator->restore_to(mBlock, mOffset);
}
