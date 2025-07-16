#include "core/utils/portable_math.h"
#include <cmath>
#include <algorithm>

namespace kubera {
namespace utils {

// Static member definitions
std::array<double, PortableMath::LUT_SIZE> PortableMath::log_lut_;
std::array<double, PortableMath::LUT_SIZE> PortableMath::exp_lut_;
std::array<double, PortableMath::LUT_SIZE> PortableMath::sqrt_lut_;
std::array<double, PortableMath::LUT_SIZE> PortableMath::sigmoid_lut_;
bool PortableMath::initialized_ = false;

void PortableMath::initialize() {
    if (initialized_) return;
    
    // Initialize lookup tables
    for (size_t i = 0; i < LUT_SIZE; ++i) {
        // Log table: 0.01 to 100.0
        double log_value = 0.01 + (static_cast<double>(i) / (LUT_SIZE - 1)) * 99.99;
        log_lut_[i] = std::log(log_value);
        
        // Exp table: -10.0 to 10.0
        double exp_value = -10.0 + (static_cast<double>(i) / (LUT_SIZE - 1)) * 20.0;
        exp_lut_[i] = std::exp(exp_value);
        
        // Sqrt table: 0.0 to 100.0
        double sqrt_value = static_cast<double>(i) / (LUT_SIZE - 1) * 100.0;
        sqrt_lut_[i] = std::sqrt(sqrt_value);
        
        // Sigmoid table: -10.0 to 10.0
        double sigmoid_value = -10.0 + (static_cast<double>(i) / (LUT_SIZE - 1)) * 20.0;
        sigmoid_lut_[i] = 1.0 / (1.0 + std::exp(-sigmoid_value));
    }
    
    initialized_ = true;
}

double PortableMath::fast_log(double x) {
    if (x <= 0.01 || x >= 100.0) return std::log(x);
    
    double normalized = (x - 0.01) / 99.99;
    size_t idx = static_cast<size_t>(normalized * (LUT_SIZE - 1));
    return log_lut_[std::min(idx, LUT_SIZE - 1)];
}

double PortableMath::fast_exp(double x) {
    if (x < -10.0 || x > 10.0) return std::exp(x);
    
    double normalized = (x + 10.0) / 20.0;
    size_t idx = static_cast<size_t>(normalized * (LUT_SIZE - 1));
    return exp_lut_[std::min(idx, LUT_SIZE - 1)];
}

double PortableMath::fast_sqrt(double x) {
    if (x < 0.0 || x >= 100.0) return std::sqrt(x);
    
    double normalized = x / 100.0;
    size_t idx = static_cast<size_t>(normalized * (LUT_SIZE - 1));
    return sqrt_lut_[std::min(idx, LUT_SIZE - 1)];
}

double PortableMath::fast_sigmoid(double x) {
    if (x < -10.0 || x > 10.0) {
        return x > 0 ? 1.0 : 0.0;
    }
    
    double normalized = (x + 10.0) / 20.0;
    size_t idx = static_cast<size_t>(normalized * (LUT_SIZE - 1));
    return sigmoid_lut_[std::min(idx, LUT_SIZE - 1)];
}

double PortableMath::fast_pow025(double x) {
    return fast_sqrt(fast_sqrt(x));
}

double PortableMath::fast_pow050(double x) {
    return fast_sqrt(x);
}

double PortableMath::fast_pow075(double x) {
    return fast_sqrt(x) * fast_sqrt(fast_sqrt(x));
}

// ✅ Batch operations for vectorized processing
void PortableMath::batch_sigmoid(const double* input, double* output, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        output[i] = fast_sigmoid(input[i]);
    }
}

void PortableMath::batch_log(const double* input, double* output, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        output[i] = fast_log(input[i]);
    }
}

} // namespace utils
} // namespace kubera
