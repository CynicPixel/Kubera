#pragma once

#include "core/utils/memory_pool.h"

namespace kubera {
namespace utils {

// Temporary alias for model compatibility
using ModelCalculationPool = MemoryPool<1024, 1024>;

} // namespace utils
} // namespace kubera
