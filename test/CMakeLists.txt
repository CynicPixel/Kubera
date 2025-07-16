find_package(GTest CONFIG REQUIRED)

# Define test source files
set(TEST_SOURCES
    test_websocket_client.cpp
    test_orderbook.cpp
    test_market_impact_model.cpp
    test_slippage_model.cpp
    test_maker_taker_model.cpp
    test_fee_calculator.cpp
    test_memory_pool.cpp
    test_thread_manager.cpp
    test_latency_tracker.cpp
    test_configuration_manager.cpp
)

# Create test executable
add_executable(kubera_tests ${TEST_SOURCES})

# Link libraries
target_link_libraries(kubera_tests PRIVATE
    GTest::gtest
    GTest::gtest_main
    Boost::system
    Boost::thread
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
    simdjson::simdjson
    Eigen3::Eigen
)

# Include directories
target_include_directories(kubera_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Add tests
add_test(NAME kubera_tests COMMAND kubera_tests)
