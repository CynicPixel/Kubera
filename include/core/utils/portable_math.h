// core/utils/portable_math.h
#pragma once

#include <cmath>
#include <array>
#include <algorithm>

namespace kubera {
namespace utils {

class PortableMath {
private:
    static constexpr size_t LUT_SIZE = 4096;
    static std::array<float, LUT_SIZE> exp_lut_;
    static std::array<float, LUT_SIZE> pow025_lut_;
    static std::array<float, LUT_SIZE> pow075_lut_;
    static std::array<float, LUT_SIZE> sigmoid_lut_;
    static bool initialized_;
    
public:
    static void initialize() {
        if (initialized_) return;
        
        for (size_t i = 0; i < LUT_SIZE; ++i) {
            float x = static_cast<float>(i) / 1000.0f;
            exp_lut_[i] = std::exp(-x); // For sigmoid
            pow025_lut_[i] = std::pow(x, 0.25f);
            pow075_lut_[i] = std::pow(x, 0.75f);
            sigmoid_lut_[i] = 1.0f / (1.0f + std::exp(-x));
        }
        
        initialized_ = true;
    }
    
    // Fast sigmoid using lookup table
    static inline float fast_sigmoid(float x) noexcept {
        x = std::max(-4.0f, std::min(4.0f, x)); // Clamp to prevent overflow
        
        if (x >= 0 && x < 4.096f) {
            size_t index = static_cast<size_t>(x * 1000.0f);
            return sigmoid_lut_[std::min(index, LUT_SIZE - 1)];
        }
        
        return 1.0f / (1.0f + std::exp(-x));
    }
    
    // Fast power functions using lookup tables
    static inline float fast_pow025(float x) noexcept {
        if (x >= 0 && x < 4.096f) {
            size_t index = static_cast<size_t>(x * 1000.0f);
            return pow025_lut_[std::min(index, LUT_SIZE - 1)];
        }
        return std::pow(x, 0.25f);
    }
    
    static inline float fast_pow075(float x) noexcept {
        if (x >= 0 && x < 4.096f) {
            size_t index = static_cast<size_t>(x * 1000.0f);
            return pow075_lut_[std::min(index, LUT_SIZE - 1)];
        }
        return std::pow(x, 0.75f);
    }
    
    // Optimized dot product (portable)
    static inline float dot_product(const float* a, const float* b, size_t size) noexcept {
        float result = 0.0f;
        
        // Unroll loop for better performance
        size_t unroll_size = size & ~3; // Round down to multiple of 4
        
        for (size_t i = 0; i < unroll_size; i += 4) {
            result += a[i] * b[i] + 
                     a[i+1] * b[i+1] + 
                     a[i+2] * b[i+2] + 
                     a[i+3] * b[i+3];
        }
        
        // Handle remaining elements
        for (size_t i = unroll_size; i < size; ++i) {
            result += a[i] * b[i];
        }
        
        return result;
    }
};

} // namespace utils
} // namespace kubera
