#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include "core/orderbook/orderbook.h"
#include "core/models/market_impact_model.h"
#include "core/models/slippage_model.h"
#include "core/models/maker_taker_model.h"
#include "core/models/fee_calculator.h"
#include "logging/logger.h"

// Forward declarations for ImGui
struct ImGuiContext;

namespace kubera {
namespace ui {

/**
 * @class UserInterface
 * @brief Implements the user interface for the trade simulator
 */

struct InputParameters {
    std::string exchange = "OKX";
    std::string spotAsset = "BTC-USDT-SWAP";
    std::string orderType = "market";
    double quantity = 100.0;
    double volatility = 0.1;
    int feeTier = 1;
};

class UserInterface {
public:
    /**
     * @brief Constructor
     * @param logger The logger instance
     */
    UserInterface(logging::Logger& logger);
    
    /**
     * @brief Destructor
     */
    ~UserInterface();
    
    /**
     * @brief Initialize the UI
     * @param windowWidth The window width
     * @param windowHeight The window height
     * @param windowTitle The window title
     * @return True if successful, false otherwise
     */
    bool initialize(int windowWidth = 1280, int windowHeight = 720, const std::string& windowTitle = "Kubera Trade Simulator");
    
    /**
     * @brief Run the UI main loop
     */
    void run();
    
    /**
     * @brief Shutdown the UI
     */
    void shutdown();
    
    /**
     * @brief Check if the UI is running
     * @return True if running, false otherwise
     */
    bool isRunning() const;
    
    /**
     * @brief Get the window handle
     * @return The window handle
     */
    void* getWindow() const;
    
    /**
     * @brief Update the order book state
     * @param orderBook The order book
     */
    void updateOrderBook(const orderbook::OrderBook& orderBook);
    
    /**
     * @brief Update the models
     * @param marketImpactModel The market impact model
     * @param slippageModel The slippage model
     * @param makerTakerModel The maker/taker model
     * @param feeCalculator The fee calculator
     */
    void updateModels(
        const models::MarketImpactModel& marketImpactModel,
        const models::SlippageModel& slippageModel,
        const models::MakerTakerModel& makerTakerModel,
        const models::FeeCalculator& feeCalculator
    );
    
    /**
     * @brief Update the model results directly
     * @param expectedSlippage The expected slippage
     * @param expectedMarketImpact The expected market impact
     * @param makerTakerProportion The maker/taker proportion
     * @param expectedFees The expected fees
     * @param netCost The net cost
     */
    void updateModelResults(
        double expectedSlippage,
        double expectedMarketImpact,
        double makerTakerProportion,
        double expectedFees,
        double netCost
    );
    
    void updateLatencyMetrics(
        double websocketLatency,
        double orderBookLatency,
        double modelCalculationLatency,
        double uiUpdateLatency
    );
    
    InputParameters getInputParameters() const;
    
    /**
     * @brief Process input events
     */
    void processEvents();
    
    /**
     * @brief Render the UI
     */
    void render();
    
private:
    // ImGui context
    ImGuiContext* imguiContext_ = nullptr;
    
    // Window handle
    void* window_ = nullptr;
    
    // Running state
    std::atomic<bool> running_{false};
    
    // Input parameters
    InputParameters inputParams_;
    
    // Output parameters
    struct OutputParameters {
        double expectedSlippage = 0.0;
        double expectedFees = 0.0;
        double expectedMarketImpact = 0.0;
        double netCost = 0.0;
        double makerTakerProportion = 0.5;
        double internalLatency = 0.0;
    };
    OutputParameters outputParams_;
    
    // Latency metrics
    struct LatencyMetrics {
        double websocketLatency = 0.0;
        double orderBookLatency = 0.0;
        double modelCalculationLatency = 0.0;
        double uiUpdateLatency = 0.0;
        double totalLatency = 0.0;
    };
    LatencyMetrics latencyMetrics_;
    
    // Order book data
    orderbook::OrderBookUpdate orderBookSnapshot_;
    orderbook::OrderBook orderBook_;
    orderbook::OrderBook dummyOrderBook_;
    bool hasValidOrderBook_ = false;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Logger
    logging::Logger& logger_;
    
    // Last update time
    std::chrono::steady_clock::time_point lastUpdateTime_;
    
    // UI state variables (replacing static variables)
    int currentExchange_ = 0;
    int currentAsset_ = 0;
    int currentOrderType_ = 0;
    std::array<float, 100> latencyHistory_ = {};
    int latencyHistoryOffset_ = 0;
    
    /**
     * @brief Render the input parameters panel
     */
    void renderInputPanel();
    
    /**
     * @brief Render the output parameters panel
     */
    void renderOutputPanel();
    
    /**
     * @brief Render the order book visualization
     */
    void renderOrderBook();
    
    /**
     * @brief Render the latency metrics
     */
    void renderLatencyMetrics();
    
    /**
     * @brief Calculate net cost
     * @return The net cost
     */
    double calculateNetCost() const;

        /**
     * @brief Render the models tab
     */
    void renderModelsTab();

    orderbook::OrderBook::PriceLevelPool priceLevelPool_;
    
};

} // namespace ui
} // namespace kubera
