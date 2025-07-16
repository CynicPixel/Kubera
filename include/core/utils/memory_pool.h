// core/utils/memory_pool.h
#pragma once

#include <memory>
#include <atomic>
#include <array>
#include <cstddef>
#include "logging/logger.h"
#include "concurrentqueue.h"

namespace kubera {
namespace utils {

// Lock-free memory pool using moodycamel for thread safety
template<size_t BlockSize, size_t BlockCount>
class OptimizedMemoryPool {
private:
    struct Block {
        std::atomic<bool> used{false};
        alignas(64) char data[BlockSize]; // Cache line alignment
        uint64_t allocation_id{0};
    };
    
    std::array<Block, BlockCount> blocks_;
    std::atomic<size_t> next_free_hint_{0};
    std::atomic<uint64_t> allocation_counter_{0};
    std::atomic<size_t> allocated_count_{0};
    
    logging::Logger& logger_;

public:
    explicit OptimizedMemoryPool(logging::Logger& logger) : logger_(logger) {
        for (auto& block : blocks_) {
            block.used.store(false, std::memory_order_relaxed);
        }
        logger_.info("Optimized memory pool initialized: {} blocks of {} bytes", 
                    BlockCount, BlockSize);
    }

    void* allocate() noexcept {
        const uint64_t alloc_id = allocation_counter_.fetch_add(1, std::memory_order_relaxed);
        size_t start_hint = next_free_hint_.load(std::memory_order_relaxed);
        
        for (size_t attempt = 0; attempt < BlockCount; ++attempt) {
            size_t index = (start_hint + attempt) % BlockCount;
            
            bool expected = false;
            if (blocks_[index].used.compare_exchange_weak(
                expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
                
                blocks_[index].allocation_id = alloc_id;
                next_free_hint_.store((index + 1) % BlockCount, std::memory_order_relaxed);
                allocated_count_.fetch_add(1, std::memory_order_relaxed);
                
                return blocks_[index].data;
            }
        }
        
        return nullptr; // Pool exhausted
    }

    bool deallocate(void* ptr) noexcept {
        if (!ptr) return false;
        
        const char* char_ptr = static_cast<const char*>(ptr);
        const char* base_ptr = reinterpret_cast<const char*>(blocks_.data());
        
        if (char_ptr < base_ptr || char_ptr >= base_ptr + sizeof(blocks_)) {
            return false;
        }
        
        const size_t offset = char_ptr - base_ptr;
        const size_t block_index = offset / sizeof(Block);
        
        if (block_index >= BlockCount) return false;
        
        Block& block = blocks_[block_index];
        if (static_cast<const char*>(ptr) != block.data) return false;
        
        block.used.store(false, std::memory_order_release);
        allocated_count_.fetch_sub(1, std::memory_order_relaxed);
        
        return true;
    }

    size_t get_allocated_count() const noexcept {
        return allocated_count_.load(std::memory_order_relaxed);
    }
};

// Keep your existing MemoryPool template
template<size_t BlockSize, size_t BlockCount>
using MemoryPool = OptimizedMemoryPool<BlockSize, BlockCount>;

} // namespace utils
} // namespace kubera
