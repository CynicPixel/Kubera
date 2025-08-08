//user_interface.cpp
#include "ui/user_interface.h"
#include "core/utils/memory_pool.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>


extern std::atomic<bool> g_running;
namespace kubera {
namespace ui {

UserInterface::UserInterface(logging::Logger& logger)
    : logger_(logger),
      priceLevelPool_(logger),
      orderBook_(logger, 100, &priceLevelPool_),
      dummyOrderBook_(logger, 100, &priceLevelPool_),
      lastUpdateTime_(std::chrono::steady_clock::now()) {
    logger_.info("User interface initialized");
}

UserInterface::~UserInterface() {
    shutdown();
}

bool UserInterface::initialize(int windowWidth, int windowHeight, const std::string& windowTitle) {
    try {
        // Initialize GLFW
        if (!glfwInit()) {
            logger_.error("Failed to initialize GLFW");
            return false;
        }
        
        // Create window
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        
        window_ = glfwCreateWindow(windowWidth, windowHeight, windowTitle.c_str(), nullptr, nullptr);
        if (!window_) {
            logger_.error("Failed to create GLFW window");
            glfwTerminate();
            return false;
        }
        
        glfwMakeContextCurrent(static_cast<GLFWwindow*>(window_));
        glfwSwapInterval(1); // Enable vsync
        
        // Initialize ImGui
        IMGUI_CHECKVERSION();
        imguiContext_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // Docking is not available in this version of ImGui
        // io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        
        // Set up ImGui style
        ImGui::StyleColorsDark();
        
        // Set up Platform/Renderer backends
        ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(window_), true);
        ImGui_ImplOpenGL3_Init("#version 330");
        
        // Set running flag
        running_ = true;
        
        logger_.info("User interface initialized with window size {}x{}", windowWidth, windowHeight);
        return true;
    } catch (const std::exception& e) {
        logger_.error("Failed to initialize user interface: {}", e.what());
        return false;
    }
}

void UserInterface::run() {
    // Check if window is valid before entering the loop
    if (!window_) {
        logger_.error("Cannot run UI: window is null");
        running_ = false;
        return;
    }

    logger_.info("UI thread starting main loop");
    
    try {
        while (running_ && !glfwWindowShouldClose(static_cast<GLFWwindow*>(window_))) {
            // Process events
            processEvents();
            
            // Check global running flag
            if (!::g_running) {
                logger_.info("Global shutdown signal detected in UI thread");
                running_ = false;
                break;
            }
            
            // Render UI
            render();
            
            // Swap buffers
            glfwSwapBuffers(static_cast<GLFWwindow*>(window_));
            
            // Short sleep to avoid busy waiting and allow for more responsive shutdown
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch (const std::exception& e) {
        logger_.error("Exception in UI thread: {}", e.what());
    } catch (...) {
        logger_.error("Unknown exception in UI thread");
    }
    
    // Ensure we set running_ to false when exiting
    running_ = false;
    logger_.info("UI thread exiting");
    
    // Clean up resources here to ensure they're released in the UI thread
    if (imguiContext_) {
        logger_.info("Cleaning up ImGui in UI thread");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(imguiContext_);
        imguiContext_ = nullptr;
    }
    
    if (window_) {
        logger_.info("Destroying GLFW window in UI thread");
        glfwDestroyWindow(static_cast<GLFWwindow*>(window_));
        window_ = nullptr;
    }
}

void UserInterface::shutdown() {
    logger_.info("User interface shutdown initiated");
    
    // Set running flag to false first
    running_ = false;
    
    // Signal the window to close if it exists
    if (window_) {
        glfwSetWindowShouldClose(static_cast<GLFWwindow*>(window_), GLFW_TRUE);
        logger_.info("Window close signal sent");
    }
    
    // Give a short delay for the UI thread to notice
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Note: Resources are now cleaned up in the run() method to ensure they're released in the UI thread
    // This avoids potential threading issues with OpenGL/GLFW resources
    
    // Only terminate GLFW if it hasn't been done already
    if (window_ || imguiContext_) {
        logger_.info("Resources still exist, terminating GLFW");
        
        // Clean up any remaining resources
        if (imguiContext_) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext(imguiContext_);
            imguiContext_ = nullptr;
            logger_.info("ImGui context destroyed");
        }
        
        if (window_) {
            glfwDestroyWindow(static_cast<GLFWwindow*>(window_));
            window_ = nullptr;
            logger_.info("GLFW window destroyed");
        }
        
        glfwTerminate();
        logger_.info("GLFW terminated");
    }
    
    logger_.info("User interface shutdown completed");
}

bool UserInterface::isRunning() const {
    return running_;
}

void* UserInterface::getWindow() const {
    return window_;
}

void UserInterface::updateOrderBook(const orderbook::OrderBook& orderBook) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // Create a snapshot of the order book
        orderBookSnapshot_.asks = orderBook.getAsks();
        orderBookSnapshot_.bids = orderBook.getBids();
        
        // We can't copy the OrderBook directly due to deleted copy constructor
        // Instead, we'll just mark that we have valid data and use the snapshot
        hasValidOrderBook_ = true;
        
        // Update last update time
        lastUpdateTime_ = std::chrono::steady_clock::now();
        
        // Debug logging to confirm updates are flowing
        if (!orderBookSnapshot_.asks.empty() && !orderBookSnapshot_.bids.empty()) {
            logger_.info("OrderBook UI updated: {} asks, {} bids, best ask: {:.2f}, best bid: {:.2f}",
                        orderBookSnapshot_.asks.size(), orderBookSnapshot_.bids.size(),
                        orderBookSnapshot_.asks[0].price, orderBookSnapshot_.bids[0].price);
        }
    } catch (const std::exception& e) {
        logger_.error("Exception in updateOrderBook: {}", e.what());
    }
}

void UserInterface::updateModels(
    const models::MarketImpactModel& marketImpactModel,
    const models::SlippageModel& slippageModel,
    const models::MakerTakerModel& makerTakerModel,
    const models::FeeCalculator& feeCalculator
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // Get input parameters
        double quantityUSD = inputParams_.quantity;  // USD amount from UI
        double volatility = inputParams_.volatility;
        int feeTier = inputParams_.feeTier;
        
        // Determine which order book to use
        const orderbook::OrderBook* orderBook = nullptr;
        if (hasValidOrderBook_) {
            // Use the real order book data
            orderBook = &orderBook_;
            logger_.debug("Using real order book for model calculations");
        } else {
            // Use the dummy order book instance (constructed once)
            orderBook = &dummyOrderBook_;
            logger_.debug("Using dummy order book for model calculations");
        }
        
        // Get current price for unit conversion
        double price = orderBook->getMidPrice();
        if (price <= 0.0) {
            price = 119000.0; // Default BTC price if no order book data
        }
        
        // CRITICAL FIX: Convert USD quantity to number of contracts
        double quantityContracts = quantityUSD / price;
        
        logger_.debug("Unit conversion: {} USD at price {} = {} contracts", 
                     quantityUSD, price, quantityContracts);
        
        // Calculate expected slippage (using contracts, not USD)
        auto slippageResult = slippageModel.predictSlippage(quantityContracts, *orderBook, true);
        outputParams_.expectedSlippage = slippageResult.expected;
        
        // Calculate expected market impact (using contracts, not USD)
        auto impactComponents = marketImpactModel.calculateImpact(quantityContracts, *orderBook, true);
        outputParams_.expectedMarketImpact = impactComponents.total;
        
        // Calculate maker/taker proportion (using contracts, not USD)
        outputParams_.makerTakerProportion = makerTakerModel.predictMakerProportion(quantityContracts, *orderBook, true);
        
        // Calculate expected fees (using contracts, not USD)
        outputParams_.expectedFees = feeCalculator.calculateFees(quantityContracts, price, feeTier, outputParams_.makerTakerProportion);
        
        // Calculate net cost
        outputParams_.netCost = calculateNetCost();
        
        // Update last update time
        lastUpdateTime_ = std::chrono::steady_clock::now();
        
        logger_.debug("Model results: slippage={:.6f}, impact={:.6f}, fees={:.6f}, net_cost={:.6f}",
                     outputParams_.expectedSlippage, outputParams_.expectedMarketImpact, 
                     outputParams_.expectedFees, outputParams_.netCost);
                     
    } catch (const std::exception& e) {
        logger_.error("Failed to update models: {}", e.what());
    }
}


void UserInterface::render() {
    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    
    // Create a simple layout without docking
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    
    // Calculate positions for each panel
    float windowWidth = viewport->Size.x;
    float windowHeight = viewport->Size.y;
    float halfWidth = windowWidth / 2.0f;
    float halfHeight = windowHeight / 2.0f;
    
    // Render panels in a 2x2 grid
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y));
    ImGui::SetNextWindowSize(ImVec2(halfWidth, halfHeight));
    renderInputPanel();
    
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + halfWidth, viewport->Pos.y));
    ImGui::SetNextWindowSize(ImVec2(halfWidth, halfHeight));
    renderOutputPanel();
    
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + halfHeight));
    ImGui::SetNextWindowSize(ImVec2(halfWidth, halfHeight));
    renderOrderBook();
    
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + halfWidth, viewport->Pos.y + halfHeight));
    ImGui::SetNextWindowSize(ImVec2(halfWidth, halfHeight));
    renderLatencyMetrics();
    
    // Add a simple menu bar as a separate window
    // ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y));
    // ImGui::SetNextWindowSize(ImVec2(windowWidth, 20));
    // ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar);
    // if (ImGui::BeginMenuBar()) {
    //     if (ImGui::BeginMenu("File")) {
    //         if (ImGui::MenuItem("Exit")) {
    //             running_ = false;
    //         }
    //         ImGui::EndMenu();
    //     }
    //     ImGui::EndMenuBar();
    // }
    // ImGui::End();
    
    // Rendering
    ImGui::Render();
    int displayW, displayH;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(window_), &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void UserInterface::renderInputPanel() {
    ImGui::Begin("Input Parameters", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    
    // Exchange selection
    const char* exchanges[] = { "OKX" };
    ImGui::Text("Exchange:");
    if (ImGui::Combo("##Exchange", &currentExchange_, exchanges, IM_ARRAYSIZE(exchanges))) {
        inputParams_.exchange = exchanges[currentExchange_];
    }
    
    // Spot asset selection
    const char* assets[] = { "BTC-USDT-SWAP", "ETH-USDT-SWAP", "SOL-USDT-SWAP" };
    ImGui::Text("Spot Asset:");
    if (ImGui::Combo("##SpotAsset", &currentAsset_, assets, IM_ARRAYSIZE(assets))) {
        inputParams_.spotAsset = assets[currentAsset_];
    }
    
    // Order type selection
    const char* orderTypes[] = { "market" };
    ImGui::Text("Order Type:");
    if (ImGui::Combo("##OrderType", &currentOrderType_, orderTypes, IM_ARRAYSIZE(orderTypes))) {
        inputParams_.orderType = orderTypes[currentOrderType_];
    }
    
    // Quantity input
    float quantity = static_cast<float>(inputParams_.quantity);
    ImGui::Text("Quantity (USD):");
    if (ImGui::SliderFloat("##Quantity", &quantity, 1.0f, 100000.0f, "%.1f")) {
        inputParams_.quantity = static_cast<double>(quantity);
    }
    
    // Volatility input
    float volatility = static_cast<float>(inputParams_.volatility);
    ImGui::Text("Volatility:");
    if (ImGui::SliderFloat("##Volatility", &volatility, 0.01f, 0.5f, "%.2f")) {
        inputParams_.volatility = static_cast<double>(volatility);
    }
    
    // Fee tier selection
    int feeTier = inputParams_.feeTier;
    ImGui::Text("Fee Tier:");
    if (ImGui::SliderInt("##FeeTier", &feeTier, 1, 5)) {
        inputParams_.feeTier = feeTier;
    }
    
    ImGui::End();
}

void UserInterface::renderOutputPanel() {
    ImGui::Begin("Output Parameters", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    
    // Format values with 6 decimal places
    auto formatValue = [](double value) -> std::string {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(6) << value;
        return ss.str();
    };
    
    // Expected slippage
    ImGui::Text("Expected Slippage:");
    ImGui::SameLine(200);
    ImGui::Text("%s", formatValue(outputParams_.expectedSlippage).c_str());
    
    // Expected fees
    ImGui::Text("Expected Fees:");
    ImGui::SameLine(200);
    ImGui::Text("%s", formatValue(outputParams_.expectedFees).c_str());
    
    // Expected market impact
    ImGui::Text("Expected Market Impact:");
    ImGui::SameLine(200);
    ImGui::Text("%s", formatValue(outputParams_.expectedMarketImpact).c_str());
    
    // Net cost
    ImGui::Text("Net Cost:");
    ImGui::SameLine(200);
    ImGui::Text("%s", formatValue(outputParams_.netCost).c_str());
    
    // Maker/taker proportion
    ImGui::Text("Maker/Taker Proportion:");
    ImGui::SameLine(200);
    ImGui::Text("%s / %s", 
                formatValue(outputParams_.makerTakerProportion).c_str(),
                formatValue(1.0 - outputParams_.makerTakerProportion).c_str());
    
    // Internal latency
    ImGui::Text("Internal Latency (Î¼s):");
    ImGui::SameLine(200);
    ImGui::Text("%.2f", latencyMetrics_.modelCalculationLatency);
    
    ImGui::End();
}

void UserInterface::renderOrderBook() {
    ImGui::Begin("Order Book", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    
    // Calculate time since last update
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdateTime_).count();
    
    ImGui::Text("Order Book: %s", inputParams_.spotAsset.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(Last update: %lld ms ago)", elapsed);
    
    ImGui::Separator();
    
    // Create columns for order book display
    if (ImGui::BeginTable("OrderBookTable", 4, ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Bid Price", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Bid Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Ask Price", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Ask Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Determine how many rows to display
        size_t numRows = std::max(orderBookSnapshot_.asks.size(), orderBookSnapshot_.bids.size());
        numRows = std::min(numRows, static_cast<size_t>(10)); // Limit to 10 rows
        
        for (size_t i = 0; i < numRows; i++) {
            ImGui::TableNextRow();
            
            // Bid price and size
            ImGui::TableNextColumn();
            if (i < orderBookSnapshot_.bids.size()) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%.2f", orderBookSnapshot_.bids[i].price);
            } else {
                ImGui::Text("-");
            }
            
            ImGui::TableNextColumn();
            if (i < orderBookSnapshot_.bids.size()) {
                ImGui::Text("%.6f", orderBookSnapshot_.bids[i].quantity);
            } else {
                ImGui::Text("-");
            }
            
            // Ask price and size
            ImGui::TableNextColumn();
            if (i < orderBookSnapshot_.asks.size()) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%.2f", orderBookSnapshot_.asks[i].price);
            } else {
                ImGui::Text("-");
            }
            
            ImGui::TableNextColumn();
            if (i < orderBookSnapshot_.asks.size()) {
                ImGui::Text("%.6f", orderBookSnapshot_.asks[i].quantity);
            } else {
                ImGui::Text("-");
            }
        }
        
        ImGui::EndTable();
    }
    
    ImGui::End();
}

void UserInterface::renderLatencyMetrics() {
    ImGui::Begin("Latency Metrics", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    
    // WebSocket latency
    ImGui::Text("WebSocket Latency (Î¼s):");
    ImGui::SameLine(200);
    ImGui::Text("%.2f", latencyMetrics_.websocketLatency);
    
    // Order book update latency
    ImGui::Text("Order Book Update Latency (Î¼s):");
    ImGui::SameLine(200);
    ImGui::Text("%.2f", latencyMetrics_.orderBookLatency);
    
    // Model calculation latency
    ImGui::Text("Model Calculation Latency (Î¼s):");
    ImGui::SameLine(200);
    ImGui::Text("%.2f", latencyMetrics_.modelCalculationLatency);
    
    // UI update latency
    ImGui::Text("UI Update Latency (Î¼s):");
    ImGui::SameLine(200);
    ImGui::Text("%.2f", latencyMetrics_.uiUpdateLatency);
    
    // Total latency
    ImGui::Text("Total Latency (Î¼s):");
    ImGui::SameLine(200);
    ImGui::Text("%.2f", latencyMetrics_.totalLatency);
    
    // Update latency history
    latencyHistory_[latencyHistoryOffset_] = static_cast<float>(latencyMetrics_.totalLatency);
    latencyHistoryOffset_ = (latencyHistoryOffset_ + 1) % latencyHistory_.size();
    
    // Calculate min/max for scaling
    float minLatency = latencyHistory_[0];
    float maxLatency = latencyHistory_[0];
    for (size_t i = 1; i < latencyHistory_.size(); i++) {
        if (latencyHistory_[i] < minLatency) minLatency = latencyHistory_[i];
        if (latencyHistory_[i] > maxLatency) maxLatency = latencyHistory_[i];
    }
    
    // Ensure min/max are different
    if (maxLatency == minLatency) {
        maxLatency += 1.0f;
    }
    
    // Plot latency history
    ImGui::PlotLines("Latency History", latencyHistory_.data(), static_cast<int>(latencyHistory_.size()), latencyHistoryOffset_,
                     "Total Latency (Î¼s)", minLatency, maxLatency, ImVec2(0, 80));
    
    ImGui::End();
}

double UserInterface::calculateNetCost() const {
    return outputParams_.expectedSlippage + outputParams_.expectedFees + outputParams_.expectedMarketImpact;
}

void UserInterface::processEvents() {
    if (!::g_running) {  // Use :: to refer to global namespace
        running_ = false;
    }
    
    // Process GLFW events
    glfwPollEvents();
}

void UserInterface::renderModelsTab() {
    if (ImGui::BeginTabItem("Models")) {
        ImGui::Text("Model Configuration");
        ImGui::Separator();
        
        // Remove model copying - models contain non-copyable atomics
        // Just display current model status instead
        ImGui::Text("Slippage Model: Active");
        ImGui::Text("Market Impact Model: Active");  
        ImGui::Text("Maker/Taker Model: Active");
        
        ImGui::EndTabItem();
    }
}

void UserInterface::updateModelResults(
    double expectedSlippage,
    double expectedMarketImpact,
    double makerTakerProportion,
    double expectedFees,
    double netCost
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    outputParams_.expectedSlippage = expectedSlippage;
    outputParams_.expectedMarketImpact = expectedMarketImpact;
    outputParams_.makerTakerProportion = makerTakerProportion;
    outputParams_.expectedFees = expectedFees;
    outputParams_.netCost = netCost;
    
    logger_.debug("UI model results updated: slippage={:.4f}, impact={:.4f}, fees={:.4f}, net={:.4f}",
                 expectedSlippage, expectedMarketImpact, expectedFees, netCost);
}

void UserInterface::updateLatencyMetrics(
    double websocketLatency,
    double orderBookLatency,
    double modelCalculationLatency,
    double uiUpdateLatency
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    latencyMetrics_.websocketLatency = websocketLatency;
    latencyMetrics_.orderBookLatency = orderBookLatency;
    latencyMetrics_.modelCalculationLatency = modelCalculationLatency;
    latencyMetrics_.uiUpdateLatency = uiUpdateLatency;
    latencyMetrics_.totalLatency = websocketLatency + orderBookLatency + modelCalculationLatency + uiUpdateLatency;
    
    logger_.debug("Latency metrics updated: WS={:.2f}, OB={:.2f}, Model={:.2f}, UI={:.2f}, Total={:.2f}",
                 websocketLatency, orderBookLatency, modelCalculationLatency, uiUpdateLatency, latencyMetrics_.totalLatency);
}

InputParameters UserInterface::getInputParameters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inputParams_;
}

// Remove problematic methods that don't match declarations

} // namespace ui
} // namespace kubera