#include "core/utils/portable_math.h"

namespace kubera {
namespace utils {

// Static member definitions
std::array<float, PortableMath::LUT_SIZE> PortableMath::exp_lut_;
std::array<float, PortableMath::LUT_SIZE> PortableMath::pow025_lut_;
std::array<float, PortableMath::LUT_SIZE> PortableMath::pow075_lut_;
std::array<float, PortableMath::LUT_SIZE> PortableMath::sigmoid_lut_;
bool PortableMath::initialized_ = false;

} // namespace utils
} // namespace kubera
