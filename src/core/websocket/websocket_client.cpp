//websocketclient.cpp
#include "core/websocket/websocket_client.h"

#include <thread>
#include <chrono>
#include <regex>
#include <iomanip>
#include <sstream>

// Namespace aliases for Boost.Beast
namespace beast = boost::beast;
namespace http = beast::http;
namespace beast_websocket = beast::websocket;
namespace net = boost::asio;
namespace ip = net::ip;
using tcp = ip::tcp;

namespace kubera {
namespace websocket {

WebSocketClient::WebSocketClient(const std::string& endpoint, logging::Logger& logger, int coreId, bool useMockMode)
    : endpoint_(endpoint), logger_(logger), coreId_(coreId) {
    
    // Configure Binance defaults if no endpoint provided
    if (endpoint_.empty()) {
        configureBinanceDefaults();
    } else {
        parseUrl(endpoint_, host_, port_, target_, symbol_);
    }
    
    logger_.info("Binance WebSocket client initialized with endpoint: {}", endpoint_);
    logger_.info("Host: {}, Port: {}, Target: {}, Symbol: {}", host_, port_, target_, symbol_);
}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

void WebSocketClient::configureBinanceDefaults() {
    endpoint_ = "wss://stream.binance.com:9443/ws/btcusdt@depth20@100ms";
    host_ = "stream.binance.com";
    port_ = "9443";
    target_ = "/ws/btcusdt@depth20@100ms";
    symbol_ = "BTC-USDT";
    
    logger_.info("Configured default Binance endpoint for BTC-USDT");
}

void WebSocketClient::parseUrl(const std::string& url, std::string& host, std::string& port,
                              std::string& target, std::string& symbol) {
    // Parse Binance WebSocket URL
    // Format: wss://stream.binance.com:9443/ws/SYMBOL@depth20@100ms
    std::regex urlRegex(R"(wss://([^:/]+)(?::(\d+))?(/ws/([^@]+)@.+))");
    std::smatch match;
    
    if (std::regex_match(url, match, urlRegex)) {
        host = match[1].str();
        port = match[2].length() > 0 ? match[2].str() : "9443";
        target = match[3].str();
        
        // Extract symbol and convert to standard format
        std::string binanceSymbol = match[4].str();
        if (binanceSymbol == "btcusdt") {
            symbol = "BTC-USDT";
        } else if (binanceSymbol == "ethusdt") {
            symbol = "ETH-USDT";
        } else if (binanceSymbol == "solusdt") {
            symbol = "SOL-USDT";
        } else {
            symbol = binanceSymbol; // Use as-is for other symbols
        }
    } else {
        logger_.warning("Failed to parse Binance WebSocket URL: {}, using defaults", url);
        configureBinanceDefaults();
    }
}

std::string WebSocketClient::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

bool WebSocketClient::connect() {
    if (connecting_ || connected_) {
        logger_.warning("Already connecting or connected to Binance WebSocket: {}", endpoint_);
        return connected_;
    }
    
    connecting_ = true;
    
    try {
        // Initialize Boost.ASIO components
        ioc_ = std::make_unique<boost::asio::io_context>();
        resolver_ = std::make_unique<tcp::resolver>(*ioc_);
        
        // Enhanced SSL context configuration for Binance
        sslCtx_ = std::make_unique<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
        sslCtx_->set_default_verify_paths();
        sslCtx_->set_verify_mode(boost::asio::ssl::verify_peer);
        sslCtx_->set_verify_callback([this](bool preverified, boost::asio::ssl::verify_context& ctx) {
            return true; // Accept Binance certificates
        });
        
        // Create SSL WebSocket stream
        ssl_ws_ = std::make_unique<beast_websocket::stream<beast::ssl_stream<tcp::socket>>>(*ioc_, *sslCtx_);
        
        // Set SNI hostname for Binance
        SSL_set_tlsext_host_name(ssl_ws_->next_layer().native_handle(), host_.c_str());
        
        // Configure WebSocket options optimized for Binance
        ssl_ws_->set_option(beast_websocket::stream_base::timeout::suggested(beast::role_type::client));
        ssl_ws_->set_option(beast_websocket::stream_base::decorator([](beast_websocket::request_type& req) {
            req.set(http::field::user_agent, "Kubera-Binance-HFT/1.0");
        }));
        
        auto& stream = beast::get_lowest_layer(*ssl_ws_);
        
        logger_.info("Resolving Binance host: {}", host_);
        auto results = resolver_->resolve(host_, port_);
        logger_.info("Connecting to Binance endpoint: {}:{}", host_, port_);
        boost::asio::connect(stream, results);
        // Set TCP options for HFT performance
        stream.set_option(tcp::no_delay(true));
        
        logger_.info("Performing SSL handshake with Binance");
        ssl_ws_->next_layer().handshake(boost::asio::ssl::stream_base::client);
        
        logger_.info("Performing WebSocket handshake with target: {}", target_);
        ssl_ws_->handshake(host_, target_);
        
        connected_ = ssl_ws_->is_open();
        logger_.info("Binance SSL WebSocket is open: {}", connected_ ? "yes" : "no");
        
        if (connected_) {
            // Start IO thread AFTER successful connection
            workGuard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(ioc_->get_executor());
            ioThread_ = std::thread([this] {
                try {
                    logger_.info("Starting IO thread");
                    ioc_->run();
                    logger_.info("IO thread exited");
                } catch (const std::exception& e) {
                    logger_.error("Exception in IO thread: {}", e.what());
                }
            });
            
            connecting_ = false;
            currentRetries_ = 0;
            
            // Start SSL read loop
            startSslReadLoop();
            handleConnection();
            
            logger_.info("Connected to Binance WebSocket endpoint: {}", endpoint_);
            return true;
        }
        
    } catch (const std::exception& e) {
        std::string errorMsg = e.what();
        logger_.error("Exception while connecting to Binance WebSocket: {}", errorMsg);
        
        // Enhanced error diagnostics for Binance
        if (errorMsg.find("handshake") != std::string::npos) {
            logger_.error("Binance WebSocket handshake failed. Possible causes:");
            logger_.error("1. SSL/TLS version mismatch with Binance");
            logger_.error("2. Certificate verification failure");
            logger_.error("3. Binance server rejected the WebSocket upgrade");
            logger_.error("4. Network connectivity to Binance blocked");
        }
        
        cleanup();
        connecting_ = false;
        return false;
    }
    
    connecting_ = false;
    return false;
}

void WebSocketClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connected_ || connecting_) {
        try {
            // Set connection state
            connected_ = false;
            connecting_ = false;
            
            // Close the SSL WebSocket connection
            if (ssl_ws_ && ssl_ws_->is_open()) {
                boost::system::error_code ec;
                ssl_ws_->close(beast_websocket::close_code::normal, ec);
                if (ec) {
                    logger_.warning("Error during Binance SSL WebSocket close: {}", ec.message());
                }
            }
            
            // Clean up resources
            cleanup();
            logger_.info("Disconnected from Binance WebSocket endpoint: {}", endpoint_);
            
        } catch (const std::exception& e) {
            logger_.error("Exception during Binance disconnection: {}", e.what());
        }
    }
}

void WebSocketClient::cleanup() {
    // Stop the IO context
    if (ioc_) {
        workGuard_.reset();
        ioc_->stop();
    }
    
    // Join the IO thread
    if (ioThread_.joinable()) {
        ioThread_.join();
    }
    
    // Reset Boost.Beast components
    ssl_ws_.reset();
    resolver_.reset();
    ioc_.reset();
}

void WebSocketClient::startSslReadLoop() {
    if (!connected_ || !ssl_ws_ || !ssl_ws_->is_open()) {
        logger_.warning("Cannot start Binance SSL read loop: connection not ready");
        return;
    }
    
    // Create a flat buffer for Binance message reading
    auto flatBuffer = std::make_shared<beast::flat_buffer>();
    flatBuffer->reserve(BUFFER_INITIAL_SIZE);
    
    // Start asynchronous read optimized for Binance
    auto self = this;
    ssl_ws_->async_read(
        *flatBuffer,
        [self, flatBuffer](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (ec) {
                if (ec == boost::beast::websocket::error::closed) {
                    self->logger_.info("Binance WebSocket connection closed gracefully");
                    self->handleDisconnection();
                } else if (ec == boost::asio::error::operation_aborted) {
                    self->logger_.info("Binance read operation was cancelled");
                } else {
                    self->logger_.error("Binance SSL read error: {} (code: {})", ec.message(), ec.value());
                    self->handleError(ec.message());
                }
                return;
            }
            
            self->logger_.debug("Binance SSL read callback: received {} bytes", bytes_transferred);
            
            try {
                // Convert buffer to string
                std::string message = boost::beast::buffers_to_string(flatBuffer->data());
                
                // Handle the Binance message
                self->handleMessage(message);
                
                // Clear the buffer for next read
                flatBuffer->consume(flatBuffer->size());
                
                // Continue reading if still connected
                if (self->connected_ && self->ssl_ws_ && self->ssl_ws_->is_open()) {
                    self->startSslReadLoop();
                } else {
                    self->logger_.warning("Stopping Binance SSL read loop: connection no longer active");
                }
                
            } catch (const std::exception& e) {
                self->logger_.error("Exception in Binance SSL read callback: {}", e.what());
                self->handleError(e.what());
            }
        });
}

void WebSocketClient::handleMessage(const std::string& message) {
    try {
        logger_.debug("Received Binance message: {}", message.substr(0, 100)); // Show first 100 chars
        
        // Parse Binance message
        auto update = parseBinanceMessage(message);
        if (update) {
            logger_.debug("Parsed Binance update: {} asks, {} bids", update->asks.size(), update->bids.size());
            
            // **CRITICAL**: Direct OrderBook update for HFT performance
            if (orderBook_) {
                orderBook_->update(*update);
                logger_.debug("Updated OrderBook with {} asks, {} bids", update->asks.size(), update->bids.size());
            }
            
            // Also enqueue for compatibility
            if (!updateQueue_.enqueue(*update)) {
                logger_.warning("Failed to enqueue Binance update - queue full");
            }
        }
        
    } catch (const std::exception& e) {
        logger_.error("Exception while handling Binance message: {}", e.what());
    }
}

std::optional<kubera::orderbook::OrderBookUpdate> WebSocketClient::parseBinanceMessage(const std::string& message) {
    try {
        logger_.debug("Parsing Binance message: {}", message.substr(0, 200) + "...");
        
        // Parse JSON using simdjson
        simdjson::dom::parser parser;
        simdjson::dom::element json = parser.parse(message);
        
        // Create update object
        kubera::orderbook::OrderBookUpdate update;
        
        // Generate missing fields that Binance doesn't provide
        update.timestamp = getCurrentTimestamp();
        update.exchange = "BINANCE";
        update.symbol = symbol_;
        
        // Parse asks array
        if (json["asks"].error() == simdjson::SUCCESS) {
            auto asksArray = json["asks"].get_array().value();
            update.asks.reserve(asksArray.size());
            
            for (auto askElement : asksArray) {
                try {
                    auto askArray = askElement.get_array().value();
                    if (askArray.size() >= 2) {
                        // Binance sends price and quantity as strings
                        std::string_view priceStr = askArray.at(0).get_string().value();
                        std::string_view quantityStr = askArray.at(1).get_string().value();
                        
                        double price = std::stod(std::string(priceStr));
                        double quantity = std::stod(std::string(quantityStr));
                        
                        update.asks.emplace_back(price, quantity);
                    } else {
                        logger_.warning("Invalid Binance ask array format: expected 2 elements, got {}", askArray.size());
                    }
                } catch (const std::exception& e) {
                    logger_.warning("Failed to parse Binance ask entry: {}", e.what());
                    continue; // Skip this entry but continue processing
                }
            }
            logger_.debug("Parsed {} Binance ask levels", update.asks.size());
        } else {
            logger_.warning("No 'asks' field found in Binance message");
        }
        
        // Parse bids array
        if (json["bids"].error() == simdjson::SUCCESS) {
            auto bidsArray = json["bids"].get_array().value();
            update.bids.reserve(bidsArray.size());
            
            for (auto bidElement : bidsArray) {
                try {
                    auto bidArray = bidElement.get_array().value();
                    if (bidArray.size() >= 2) {
                        // Binance sends price and quantity as strings
                        std::string_view priceStr = bidArray.at(0).get_string().value();
                        std::string_view quantityStr = bidArray.at(1).get_string().value();
                        
                        double price = std::stod(std::string(priceStr));
                        double quantity = std::stod(std::string(quantityStr));
                        
                        update.bids.emplace_back(price, quantity);
                    } else {
                        logger_.warning("Invalid Binance bid array format: expected 2 elements, got {}", bidArray.size());
                    }
                } catch (const std::exception& e) {
                    logger_.warning("Failed to parse Binance bid entry: {}", e.what());
                    continue; // Skip this entry but continue processing
                }
            }
            logger_.debug("Parsed {} Binance bid levels", update.bids.size());
        } else {
            logger_.warning("No 'bids' field found in Binance message");
        }
        
        // Validate that we have meaningful data
        if (update.asks.empty() && update.bids.empty()) {
            logger_.warning("Parsed Binance message contains no orderbook data");
            return std::nullopt;
        }
        
        logger_.debug("Successfully parsed Binance orderbook update: {} asks, {} bids for {}",
                     update.asks.size(), update.bids.size(), update.symbol);
        return update;
        
    } catch (const simdjson::simdjson_error& e) {
        logger_.error("simdjson parsing error for Binance message: {}", e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        logger_.error("Failed to parse Binance message: {}", e.what());
        logger_.debug("Binance message content: {}", message);
        return std::nullopt;
    }
}

bool WebSocketClient::isConnected() const {
    return connected_ && ssl_ws_ && ssl_ws_->is_open();
}

void WebSocketClient::setOrderBook(std::shared_ptr<orderbook::OrderBook> orderBook) {
    orderBook_ = orderBook;
    logger_.info("OrderBook linked to Binance WebSocket client for direct HFT updates");
}

std::optional<kubera::orderbook::OrderBookUpdate> WebSocketClient::getNextUpdate() {
    kubera::orderbook::OrderBookUpdate update;
    if (updateQueue_.try_dequeue(update)) {
        return update;
    }
    return std::nullopt;
}

void WebSocketClient::processMessages() {
    // This method is kept for API compatibility
    // In optimized version, messages are processed asynchronously
    // For now, just return - the read loop handles message processing
    return;
}

void WebSocketClient::handleError(const std::string& error) {
    logger_.error("Binance WebSocket error: {}", error);
    // Attempt to reconnect
    if (connected_) {
        connected_ = false;
        reconnect();
    }
}

void WebSocketClient::handleConnection() {
    logger_.info("Binance WebSocket connected");
    connected_ = true;
    currentRetries_ = 0;
}

void WebSocketClient::handleDisconnection() {
    logger_.info("Binance WebSocket disconnected");
    connected_ = false;
    // Attempt to reconnect
    reconnect();
}

bool WebSocketClient::reconnect() {
    if (currentRetries_ >= maxRetries_) {
        logger_.error("Maximum Binance reconnection attempts reached");
        return false;
    }
    
    // Calculate delay
    int delay = calculateReconnectDelay();
    logger_.info("Reconnecting to Binance in {} ms (attempt {}/{})", delay, currentRetries_ + 1, maxRetries_);
    
    // Sleep
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    
    // Increment retry counter
    currentRetries_++;
    
    // Clean up old connection
    cleanup();
    
    // Reconnect
    return connect();
}

int WebSocketClient::calculateReconnectDelay() const {
    int delay = initialDelayMs_;
    for (int i = 0; i < currentRetries_; i++) {
        delay = static_cast<int>(delay * backoffMultiplier_);
        if (delay > maxDelayMs_) {
            delay = maxDelayMs_;
            break;
        }
    }
    return delay;
}

void WebSocketClient::setReconnectionOptions(
    int maxRetries,
    int initialDelayMs,
    int maxDelayMs,
    double backoffMultiplier
) {
    maxRetries_ = maxRetries;
    initialDelayMs_ = initialDelayMs;
    maxDelayMs_ = maxDelayMs;
    backoffMultiplier_ = backoffMultiplier;
    logger_.info("Binance reconnection options set: maxRetries={}, initialDelayMs={}, maxDelayMs={}, backoffMultiplier={}",
                maxRetries_, initialDelayMs_, maxDelayMs_, backoffMultiplier_);
}

} // namespace websocket
} // namespace kubera
