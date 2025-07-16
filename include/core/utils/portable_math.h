#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace kubera {
namespace utils {

class PortableMath {
private:
    static constexpr size_t LUT_SIZE = 10000;
    static std::array<double, LUT_SIZE> log_lut_;
    static std::array<double, LUT_SIZE> exp_lut_;
    static std::array<double, LUT_SIZE> sqrt_lut_;
    static std::array<double, LUT_SIZE> sigmoid_lut_;
    static bool initialized_;

public:
    static void initialize();
    
    // ✅ Fast mathematical operations
    static double fast_log(double x);
    static double fast_exp(double x);
    static double fast_sqrt(double x);
    static double fast_sigmoid(double x);
    static double fast_pow025(double x);
    static double fast_pow050(double x);
    static double fast_pow075(double x);
    
    // ✅ Vectorized operations for batch processing
    static void batch_sigmoid(const double* input, double* output, size_t count);
    static void batch_log(const double* input, double* output, size_t count);
};

} // namespace utils
} // namespace kubera
