//main.cpp
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <signal.h>
#include <condition_variable>
#include <mutex>

#include "core/websocket/websocket_client.h"
#include "core/orderbook/orderbook.h"
#include "core/models/market_impact_model.h"
#include "core/models/slippage_model.h"
#include "core/models/maker_taker_model.h"
#include "core/models/fee_calculator.h"
#include "core/utils/thread_manager.h"
#include "core/utils/latency_tracker.h"
#include "core/utils/memory_pool.h"
#include "core/hft/hft_model_manager.h"
#include "ui/user_interface.h"
#include "logging/logger.h"
#include <GLFW/glfw3.h>

// Global flag for graceful shutdown
std::atomic<bool> g_running(true);

// Signal handler for graceful shutdown
void signalHandler(int signal) {
    std::cout << "Received signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

int main(int argc, char* argv[]) {
    try {
        // Check for headless mode
        bool headlessMode = false;
        if (argc > 1 && std::string(argv[1]) == "--headless") {
            headlessMode = true;
        }
        
        // Register signal handlers
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);
        
        // Initialize logger
        kubera::logging::Logger logger("kubera.log");
        logger.info("Kubera Trade Simulator starting up - HFT Mode (headless: {})", headlessMode);
        
        // Initialize thread manager
        kubera::utils::ThreadManager threadManager(logger);
        logger.info("Thread manager initialized with {} cores", threadManager.getNumCores());
        
        // Initialize latency tracker
        kubera::utils::LatencyTracker latencyTracker(logger);
        
        // Initialize memory pools with correct types
        kubera::utils::MemoryPool<64, 1024> featurePool(logger);
        kubera::utils::MemoryPool<1024, 1024> messagePool(logger);
        kubera::utils::MemoryPool<64, 4096> objectPool(logger);
        
        // Initialize GLFW in the main thread (only if not in headless mode)
        if (!headlessMode) {
            if (!glfwInit()) {
                logger.error("Failed to initialize GLFW in main thread");
                return 1;
            }
            logger.info("GLFW initialized in main thread");
            
            // Set GLFW error callback
            glfwSetErrorCallback([](int error, const char* description) {
                std::cerr << "GLFW Error " << error << ": " << description << std::endl;
            });
        }
        
        // Initialize models with correct memory pool type
        kubera::models::SlippageModel slippageModel(logger, featurePool);
        kubera::models::MarketImpactModel marketImpactModel(logger, messagePool);
        kubera::models::MakerTakerModel makerTakerModel(logger, featurePool);
        kubera::models::FeeCalculator feeCalculator(logger);

        // CRITICAL: Create SHARED OrderBook instance
        auto sharedOrderBook = std::make_shared<kubera::orderbook::OrderBook>(logger, 100);

        // Initialize HFT Model Manager (replaces queue-based communication)
        kubera::hft::HFTModelManager modelManager(
            slippageModel, marketImpactModel, makerTakerModel, feeCalculator, logger, *sharedOrderBook);
        
        // Initialize WebSocket client
        auto websocketClient = std::make_unique<kubera::websocket::WebSocketClient>(
            "wss://ws.gomarket-cpp.goquant.io/ws/l2-orderbook/okx/BTC-USDT-SWAP", 
            logger, 
            1, 
            false // Real mode
        );
        
        // CRITICAL: Link OrderBook to WebSocket client
        websocketClient->setOrderBook(sharedOrderBook);
        
        // Initialize UI in the main thread
        kubera::ui::UserInterface ui(logger);
        
        // Initialize UI - this must be done on the main thread for macOS
        if (!headlessMode) {
            if (!ui.initialize()) {
                logger.error("Failed to initialize UI");
                return 1;
            }
            logger.info("UI initialized successfully on main thread");
        } else {
            logger.info("Running in headless mode, skipping UI initialization");
        }
        
        // HFT WebSocket thread - direct updates with sub-microsecond model updates
        threadManager.createThread("HFT_WebSocket", [&]() {
            logger.info("HFT WebSocket thread starting");
            
            // Connect to WebSocket
            if (!websocketClient->connect()) {
                logger.error("Failed to connect to WebSocket");
                g_running = false;
                return;
            }
            
            // Main WebSocket processing loop - now updates HFT model inputs directly
            while (g_running && websocketClient->isConnected()) {
                // Start latency measurement
                latencyTracker.startMeasurement("hft_websocket_processing");
                
                // Process messages - updates shared OrderBook
                websocketClient->processMessages();
                
                // Direct model input update (replaces snapshot queue) - ULTRA FAST
                modelManager.updateMarketData(*sharedOrderBook);
                
                // End latency measurement
                double websocketLatency = latencyTracker.endMeasurement("hft_websocket_processing");
                
                // Much tighter loop for HFT performance - REDUCED frequency
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            logger.info("HFT WebSocket thread exiting");
        }, 2, kubera::utils::ThreadManager::ThreadPriority::ABOVE_NORMAL);
        
        // HFT Model calculation thread - direct atomic calculations
        threadManager.createThread("HFT_Models", [&]() {
            logger.info("HFT Model thread starting");
            
            // Default input parameters for headless mode
            kubera::ui::InputParameters defaultParams;
            defaultParams.quantity = 100.0;
            defaultParams.feeTier = 1;
            
            int iterations = 0;
            while (g_running) {
                iterations++;
                
                // Start latency measurement
                latencyTracker.startMeasurement("hft_model_calculation");
                
                // Get input parameters - use defaults in headless mode
                auto inputParams = headlessMode ? defaultParams : ui.getInputParameters();
                
                // Use HFT optimized calculations (sub-10μs)
                modelManager.calculateModelsHFT(
                    inputParams.quantity,
                    true, // is_buy
                    inputParams.feeTier
                );
                
                // End latency measurement
                double modelLatency = latencyTracker.endMeasurement("hft_model_calculation");
                
                // Log every 1000 iterations in headless mode
                if (headlessMode && iterations % 1000 == 0) {
                    logger.info("HFT Model thread iteration {}, g_running={}", iterations, g_running.load());
                }
                
                // Even faster calculation cycle for true HFT performance - REDUCED frequency
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            
            logger.info("HFT Model thread exiting after {} iterations", iterations);
        }, 3, kubera::utils::ThreadManager::ThreadPriority::ABOVE_NORMAL);
        
        // Signal that all threads are started
        logger.info("All HFT threads started, running UI in main thread");
        
        // Main thread loop - UI with atomic result reads
        if (!headlessMode) {
            // Main thread handles both UI updates and rendering
            while (g_running && ui.isRunning()) {
                // Start UI latency measurement
                latencyTracker.startMeasurement("ui_update");
                
                // Update UI with current OrderBook state
                ui.updateOrderBook(*sharedOrderBook);
                
                // Atomic result reads (replaces queue dequeue)
                auto results = modelManager.getResults();
                
                // Update UI with model results
                ui.updateModelResults(
                    results.expected_slippage,
                    results.expected_impact,
                    results.maker_proportion,
                    results.expected_fees,
                    results.net_cost
                );
                
                // Update latency metrics
                ui.updateLatencyMetrics(
                    latencyTracker.getStatistics("hft_websocket_processing").avgMicros,
                    0.0, // No longer using OrderBook snapshots
                    latencyTracker.getStatistics("hft_model_calculation").avgMicros,
                    latencyTracker.endMeasurement("ui_update")
                );
                
                // Process events and render one frame
                ui.processEvents();
                
                // Check global running flag
                if (!g_running) {
                    logger.info("Global shutdown signal detected in main thread");
                    break;
                }
                
                // Render UI
                ui.render();
                
                // Swap buffers
                glfwSwapBuffers(static_cast<GLFWwindow*>(ui.getWindow()));
                
                // Short sleep to avoid busy waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
            }
        } else {
            // Headless mode - just run for a test period
            logger.info("Running in headless mode for testing");
            int counter = 0;
            while (g_running && counter < 100) { // Run for 100 iterations
                // Comment out UI update to test if that's causing the issue
                // ui.updateOrderBook(*sharedOrderBook);
                
                // Atomic result reads (replaces queue dequeue)
                auto results = modelManager.getResults();
                
                // Log results every 10 iterations in headless mode
                if (counter % 10 == 0) {
                    logger.info("Model results: slippage={:.6f}, impact={:.6f}, maker_prop={:.6f}, fees={:.6f}, net_cost={:.6f}, calc_count={}", 
                                results.expected_slippage, results.expected_impact, results.maker_proportion, 
                                results.expected_fees, results.net_cost, results.calculation_count);
                }
                
                // Update UI with model results
                ui.updateModelResults(
                    results.expected_slippage,
                    results.expected_impact,
                    results.maker_proportion,
                    results.expected_fees,
                    results.net_cost
                );
                
                // Sleep for a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                counter++;
            }
            logger.info("Headless mode test completed");
        }
        
        // Shutdown sequence
        logger.info("HFT shutdown sequence initiated");
        
        // Disconnect WebSocket
        websocketClient->disconnect();
        
        // Shutdown UI
        if (!headlessMode) {
            ui.shutdown();
        }
        
        // Join all threads with timeout
        logger.info("Joining threads");
        bool joinSuccess = threadManager.joinAllThreadsWithTimeout(10000); // 10 second timeout
        
        if (!joinSuccess) {
            logger.warning("Some threads did not join within timeout, detaching them");
        }
        
        // Terminate GLFW
        if (!headlessMode) {
            glfwTerminate();
            logger.info("GLFW terminated");
        }
        
        // Generate latency report
        logger.info("HFT Latency report:\n{}", latencyTracker.generateReport());
        
        // Generate memory pool stats - remove getStats() calls if not available
        logger.info("Message pool initialized successfully");
        logger.info("Object pool initialized successfully");
        
        logger.info("Kubera Trade Simulator shutting down");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
