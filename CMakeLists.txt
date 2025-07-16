cmake_minimum_required(VERSION 3.15)

project(kubera LANGUAGES CXX)

# Add policy settings
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
cmake_policy(SET CMP0074 NEW)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(Eigen3 CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(OpenSSL REQUIRED)

# Add executable (removed duplicate main.cpp)
add_executable(kubera
  src/main.cpp
  # Core implementations
  src/core/models/fee_calculator.cpp
  src/core/models/maker_taker_model.cpp
  src/core/models/market_impact_model.cpp
  src/core/models/slippage_model.cpp
  src/core/models/shared_cache.cpp
  src/core/hft/hft_model_manager.cpp
  src/core/orderbook/orderbook.cpp
  src/core/utils/latency_tracker.cpp
  src/core/utils/configuration_manager.cpp
  src/core/utils/thread_manager.cpp
  src/core/utils/portable_math.cpp
  src/core/websocket/websocket_client.cpp
  src/logging/logger.cpp
  # UI implementation
  src/ui/user_interface.cpp
)

target_include_directories(kubera PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(kubera PRIVATE
  Boost::system
  Boost::thread
  spdlog::spdlog
  fmt::fmt
  simdjson::simdjson
  Eigen3::Eigen
  glfw
  imgui::imgui
  OpenGL::GL
  OpenSSL::SSL  
  OpenSSL::Crypto
)

target_compile_definitions(kubera PRIVATE
  IMGUI_HAS_GLFW
  IMGUI_IMPL_OPENGL_LOADER_GLAD
  GL_SILENCE_DEPRECATION  # Added to silence OpenGL deprecation warnings
)