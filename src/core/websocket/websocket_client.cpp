//websocketclient.cpp
#include "core/websocket/websocket_client.h"
#include <thread>
#include <chrono>
#include <regex>
#include <optional>
#include <charconv>
#include <simdjson.h>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>

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
    : endpoint_(endpoint), logger_(logger), coreId_(coreId), useMockMode_(useMockMode) {
    
    // Parse the WebSocket URL
    parseUrl(endpoint_, host_, port_, target_, secure_);
    
    if (useMockMode_) {
        logger_.info("WebSocket client initialized in MOCK MODE with endpoint: {}", endpoint_);
    } else {
        logger_.info("WebSocket client initialized with endpoint: {}", endpoint_);
        logger_.info("Host: {}, Port: {}, Target: {}, Secure: {}", host_, port_, target_, secure_ ? "true" : "false");
    }
}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

void WebSocketClient::parseUrl(const std::string& url, std::string& host, std::string& port, 
                              std::string& target, bool& secure) {
    // Regular expression to parse WebSocket URL
    // Format: (ws|wss)://hostname[:port][/path]
    std::regex urlRegex(R"((ws|wss)://([^:/]+)(?::(\d+))?(/.*))");
    std::smatch match;
    
    if (std::regex_match(url, match, urlRegex)) {
        std::string scheme = match[1].str();
        host = match[2].str();
        port = match[3].length() > 0 ? match[3].str() : (scheme == "wss" ? "443" : "80");
        target = match[4].str();
        secure = (scheme == "wss");
    } else {
        // Default values if URL parsing fails
        host = "localhost";
        port = "80";
        target = "/";
        secure = false;
        logger_.warning("Failed to parse WebSocket URL: {}, using defaults", url);
    }
}

bool WebSocketClient::connect() {
    if (connecting_ || connected_) {
        logger_.warning("Already connecting or connected to WebSocket endpoint: {}", endpoint_);
        return connected_;
    }

    connecting_ = true;

    if (useMockMode_) {
        // Mock mode logic
        return true;
    }

    try {
        ioc_ = std::make_unique<net::io_context>();
        resolver_ = std::make_unique<tcp::resolver>(*ioc_);

        if (secure_) {
            // Enhanced SSL context configuration
            sslCtx_ = std::make_unique<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
            
            // Set proper SSL options
            sslCtx_->set_default_verify_paths();
            sslCtx_->set_verify_mode(boost::asio::ssl::verify_peer);
            sslCtx_->set_verify_callback([this](bool preverified, boost::asio::ssl::verify_context& ctx) {
                return true; // Accept all certificates
            });
            
            // CREATE the SSL WebSocket FIRST
            ssl_ws_ = std::make_unique<beast_websocket::stream<boost::asio::ssl::stream<beast::tcp_stream>>>(*ioc_, *sslCtx_);
            
            // THEN set SNI hostname
            SSL_set_tlsext_host_name(ssl_ws_->next_layer().native_handle(), host_.c_str());
            
            // Configure WebSocket options
            ssl_ws_->set_option(beast_websocket::stream_base::timeout::suggested(beast::role_type::client));
            ssl_ws_->set_option(beast_websocket::stream_base::decorator([](beast_websocket::request_type& req) {
                req.set(http::field::user_agent, "Kubera-WebSocket-Client/1.0");
            }));

            auto& stream = beast::get_lowest_layer(*ssl_ws_);
            
            logger_.info("Resolving host: {}", host_);
            auto const results = resolver_->resolve(host_, port_);
            
            logger_.info("Connecting to endpoint: {}:{}", host_, port_);
            stream.connect(results);
            
            // Set TCP options
            stream.socket().set_option(tcp::no_delay(true));
            
            logger_.info("Performing SSL handshake");
            ssl_ws_->next_layer().handshake(boost::asio::ssl::stream_base::client);
            
            logger_.info("Performing WebSocket handshake with target: {}", target_);
            ssl_ws_->handshake(host_, target_);
            
            logger_.info("SSL WebSocket is open: {}", ssl_ws_->is_open() ? "yes" : "no");
        } else {
            // Non-SSL WebSocket logic (unchanged)
            ws_ = std::make_unique<beast_websocket::stream<beast::tcp_stream>>(*ioc_);
            auto& stream = beast::get_lowest_layer(*ws_);
            
            logger_.info("Resolving host: {}", host_);
            auto const results = resolver_->resolve(host_, port_);
            
            logger_.info("Connecting to endpoint: {}:{}", host_, port_);
            stream.connect(results);
            stream.socket().set_option(tcp::no_delay(true));
            
            logger_.info("Performing WebSocket handshake with target: {}", target_);
            ws_->handshake(host_, target_);
            
            logger_.info("WebSocket is open: {}", ws_->is_open() ? "yes" : "no");
        }
        
        // Start IO thread AFTER successful connection
        workGuard_ = std::make_unique<net::executor_work_guard<net::io_context::executor_type>>(ioc_->get_executor());
        ioThread_ = std::thread([this] {
            try {
                logger_.info("Starting IO thread");
                ioc_->run();
                logger_.info("IO thread exited");
            } catch (const std::exception& e) {
                logger_.error("Exception in IO thread: {}", e.what());
            }
        });


        connected_ = true;
        connecting_ = false;
        currentRetries_ = 0;
       
        
        // Start appropriate read loop
        if (secure_) {
            startSslReadLoop();
        } else {
            startReadLoop();
        }
        
         handleConnection();
        

        logger_.info("Connected to WebSocket endpoint: {}", endpoint_);
        return true;
        
    } catch (const std::exception& e) {
        std::string errorMsg = e.what();
        logger_.error("Exception while connecting to WebSocket endpoint: {}", errorMsg);
        
        // Enhanced error diagnostics
        if (errorMsg.find("handshake") != std::string::npos) {
            logger_.error("WebSocket handshake failed. Possible causes:");
            logger_.error("1. SSL/TLS version mismatch");
            logger_.error("2. Certificate verification failure");
            logger_.error("3. Server rejected the WebSocket upgrade request");
            logger_.error("4. Network connectivity issues");
        }
        
        cleanup();
        connecting_ = false;
        return false;
    }
}


bool WebSocketClient::sendTestMessage() {
    if (useMockMode_) {
        logger_.info("Mock mode enabled, simulating test message");
        return true;
    }
    
    if (!isConnected()) {
        logger_.error("Cannot send test message: not connected");
        return false;
    }
    logger_.info("Connected to pre-configured gateway - no subscription message needed");
    return true;
   
}

void WebSocketClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Handle mock mode
    if (useMockMode_) {
        // Stop the mock thread
        mockRunning_ = false;
        if (mockThread_.joinable()) {
            mockThread_.join();
        }
        connected_ = false;
        connecting_ = false;
        logger_.info("Disconnected from mock WebSocket endpoint");
        return;
    }
    
    if (connected_ || connecting_) {
        try {
            // Set connection state
            connected_ = false;
            connecting_ = false;
            
            // Close the WebSocket connection
            if (secure_ && ssl_ws_ && ssl_ws_->is_open()) {
                boost::system::error_code ec;
                ssl_ws_->close(beast_websocket::close_code::normal, ec);
                if (ec) {
                    logger_.warning("Error during SSL WebSocket close: {}", ec.message());
                }
            } else if (!secure_ && ws_ && ws_->is_open()) {
                boost::system::error_code ec;
                ws_->close(beast_websocket::close_code::normal, ec);
                if (ec) {
                    logger_.warning("Error during WebSocket close: {}", ec.message());
                }
            }
            
            // Clean up resources
            cleanup();
            
            logger_.info("Disconnected from WebSocket endpoint: {}", endpoint_);
        } catch (const std::exception& e) {
            logger_.error("Exception during disconnection: {}", e.what());
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
    ws_.reset();
    resolver_.reset();
    ioc_.reset();
}

std::optional<kubera::orderbook::OrderBookUpdate> WebSocketClient::getNextUpdate() {
    kubera::orderbook::OrderBookUpdate update;
    if (updateQueue_.try_dequeue(update)) {
        return update;
    }
    return std::nullopt;
}

bool WebSocketClient::isConnected() const {
    if (useMockMode_) {
        return connected_;
    }
    if (secure_) {
        return connected_ && ssl_ws_ && ssl_ws_->is_open();
    } else {
        return connected_ && ws_ && ws_->is_open();
    }
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
    
    logger_.info("Reconnection options set: maxRetries={}, initialDelayMs={}, maxDelayMs={}, backoffMultiplier={}",
               maxRetries_, initialDelayMs_, maxDelayMs_, backoffMultiplier_);
}

void WebSocketClient::startReadLoop() {
    if (!connected_ || !ws_ || !ws_->is_open()) {
        return;
    }
    
    // Create a flat buffer
    auto flatBuffer = std::make_shared<beast::flat_buffer>();
    
    // Start asynchronous read
    auto self = this; // Capture 'this' to ensure it remains valid
    ws_->async_read(
        *flatBuffer,
        [self, flatBuffer](boost::system::error_code ec, std::size_t bytes_transferred) {
            self->logger_.critical("READ CALLBACK ENTRY - bytes: {}", bytes_transferred);
            if (ec) {
                if (ec != beast_websocket::error::closed) {
                    self->handleError(ec.message());
                }
                return;
            }
            
            // Process the message
            std::string message(
                static_cast<char const*>(flatBuffer->data().data()),
                flatBuffer->size());
            
            // Handle the message
            self->handleMessage(message);
            
            // Clear the buffer
            flatBuffer->consume(flatBuffer->size());
            
            // Continue reading if still connected
            if (self->connected_ && self->ws_ && self->ws_->is_open()) {
                self->startReadLoop();
            }
        });
}

void WebSocketClient::startSslReadLoop() {
    if (!connected_ || !ssl_ws_ || !ssl_ws_->is_open()) {
        logger_.warning("Cannot start SSL read loop: connection not ready");
        return;
    }
    
    // Create a flat buffer with appropriate size
    auto flatBuffer = std::make_shared<boost::beast::flat_buffer>();
    flatBuffer->reserve(BUFFER_INITIAL_SIZE);
    
    // Start asynchronous read with proper error handling
    auto self = this;
    ssl_ws_->async_read(
        *flatBuffer,
        [self, flatBuffer](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (ec) {
                if (ec == boost::beast::websocket::error::closed) {
                    self->logger_.info("WebSocket connection closed gracefully");
                    self->handleDisconnection();
                } else if (ec == boost::asio::error::operation_aborted) {
                    self->logger_.info("Read operation was cancelled");
                } else {
                    self->logger_.error("SSL read error: {} (code: {})", ec.message(), ec.value());
                    self->handleError(ec.message());
                }
                return;
            }
            
            self->logger_.debug("SSL read callback: received {} bytes", bytes_transferred);
            
            try {
                // Convert buffer to string
                std::string message = boost::beast::buffers_to_string(flatBuffer->data());
                
                // Handle the message
                self->handleMessage(message);
                
                // Clear the buffer for next read
                flatBuffer->consume(flatBuffer->size());
                
                // Continue reading if still connected
                if (self->connected_ && self->ssl_ws_ && self->ssl_ws_->is_open()) {
                    self->startSslReadLoop();
                } else {
                    self->logger_.warning("Stopping SSL read loop: connection no longer active");
                }
            } catch (const std::exception& e) {
                self->logger_.error("Exception in SSL read callback: {}", e.what());
                self->handleError(e.what());
            }
        });
}

void WebSocketClient::handleMessage(const std::string& message) {
    try {
        logger_.debug("Received message: {}", message.substr(0, 100)); // Show first 100 chars
        
        // Parse message
        auto update = parseMessage(message);
        
        if (update) {
            logger_.debug("Parsed update: {} asks, {} bids", update->asks.size(), update->bids.size());
            
            // **CRITICAL FIX**: Update OrderBook directly for HFT performance
            if (orderBook_) {
                orderBook_->update(*update);
                logger_.debug("Updated OrderBook with {} asks, {} bids", update->asks.size(), update->bids.size());
            }
            
            // Also enqueue for compatibility
            if (!updateQueue_.enqueue(*update)) {
                logger_.warning("Failed to enqueue update");
            }
        }
    } catch (const std::exception& e) {
        logger_.error("Exception while handling message: {}", e.what());
    }
}

void WebSocketClient::handleError(const std::string& error) {
    logger_.error("WebSocket error: {}", error);
    
    // Attempt to reconnect
    if (connected_) {
        connected_ = false;
        reconnect();
    }
}

void WebSocketClient::handleConnection() {
    logger_.info("WebSocket connected");
    connected_ = true;
    currentRetries_ = 0;
}

void WebSocketClient::handleDisconnection() {
    logger_.info("WebSocket disconnected");
    connected_ = false;
    
    // Attempt to reconnect
    reconnect();
}

bool WebSocketClient::reconnect() {
    if (currentRetries_ >= maxRetries_) {
        logger_.error("Maximum reconnection attempts reached");
        return false;
    }
    
    // Calculate delay
    int delay = calculateReconnectDelay();
    
    logger_.info("Reconnecting in {} ms (attempt {}/{})", delay, currentRetries_ + 1, maxRetries_);
    
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

std::optional<kubera::orderbook::OrderBookUpdate> WebSocketClient::parseMessage(const std::string& message) {
    try {
        logger_.debug("Parsing OKX message: {}", message.substr(0, 200) + "...");
        
        // Parse JSON using simdjson
        simdjson::dom::parser parser;
        simdjson::dom::element json = parser.parse(message);
        
        // Create update object
        kubera::orderbook::OrderBookUpdate update;
        
        // Parse timestamp (required field)
        if (json["timestamp"].error() == simdjson::SUCCESS) {
            std::string_view timestampStr = json["timestamp"].get_string().value();
            update.timestamp = std::string(timestampStr);
        } else {
            logger_.error("Missing required 'timestamp' field in OKX message");
            return std::nullopt;
        }
        
        // Parse exchange (required field)
        if (json["exchange"].error() == simdjson::SUCCESS) {
            std::string_view exchangeStr = json["exchange"].get_string().value();
            update.exchange = std::string(exchangeStr);
        } else {
            logger_.error("Missing required 'exchange' field in OKX message");
            return std::nullopt;
        }
        
        // Parse symbol (required field)
        if (json["symbol"].error() == simdjson::SUCCESS) {
            std::string_view symbolStr = json["symbol"].get_string().value();
            update.symbol = std::string(symbolStr);
        } else {
            logger_.error("Missing required 'symbol' field in OKX message");
            return std::nullopt;
        }
        
        // Parse asks array
        if (json["asks"].error() == simdjson::SUCCESS) {
            auto asksArray = json["asks"].get_array().value();
            update.asks.reserve(asksArray.size()); // Pre-allocate for performance
            
            for (auto askElement : asksArray) {
                try {
                    auto askArray = askElement.get_array().value();
                    if (askArray.size() >= 2) {
                        // OKX sends price and quantity as strings
                        std::string_view priceStr = askArray.at(0).get_string().value();
                        std::string_view quantityStr = askArray.at(1).get_string().value();
                        
                        double price = std::stod(std::string(priceStr));
                        double quantity = std::stod(std::string(quantityStr));
                        
                        update.asks.emplace_back(price, quantity);
                    } else {
                        logger_.warning("Invalid ask array format: expected 2 elements, got {}", askArray.size());
                    }
                } catch (const std::exception& e) {
                    logger_.warning("Failed to parse ask entry: {}", e.what());
                    continue; // Skip this entry but continue processing
                }
            }
            logger_.debug("Parsed {} ask levels", update.asks.size());
        } else {
            logger_.warning("No 'asks' field found in OKX message");
        }
        
        // Parse bids array
        if (json["bids"].error() == simdjson::SUCCESS) {
            auto bidsArray = json["bids"].get_array().value();
            update.bids.reserve(bidsArray.size()); // Pre-allocate for performance
            
            for (auto bidElement : bidsArray) {
                try {
                    auto bidArray = bidElement.get_array().value();
                    if (bidArray.size() >= 2) {
                        // OKX sends price and quantity as strings
                        std::string_view priceStr = bidArray.at(0).get_string().value();
                        std::string_view quantityStr = bidArray.at(1).get_string().value();
                        
                        double price = std::stod(std::string(priceStr));
                        double quantity = std::stod(std::string(quantityStr));
                        
                        update.bids.emplace_back(price, quantity);
                    } else {
                        logger_.warning("Invalid bid array format: expected 2 elements, got {}", bidArray.size());
                    }
                } catch (const std::exception& e) {
                    logger_.warning("Failed to parse bid entry: {}", e.what());
                    continue; // Skip this entry but continue processing
                }
            }
            logger_.debug("Parsed {} bid levels", update.bids.size());
        } else {
            logger_.warning("No 'bids' field found in OKX message");
        }
        
        // Validate that we have meaningful data
        if (update.asks.empty() && update.bids.empty()) {
            logger_.warning("Parsed OKX message contains no orderbook data");
            return std::nullopt;
        }
        
        logger_.debug("Successfully parsed OKX orderbook update: {} asks, {} bids for {}", 
                    update.asks.size(), update.bids.size(), update.symbol);
        
        return update;
        
    } catch (const simdjson::simdjson_error& e) {
        logger_.error("simdjson parsing error: {}", e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        logger_.error("Failed to parse OKX message: {}", e.what());
        logger_.debug("Message content: {}", message);
        return std::nullopt;
    }
}

void WebSocketClient::setOrderBook(std::shared_ptr<kubera::orderbook::OrderBook> orderBook) {
    orderBook_ = orderBook;
    logger_.info("OrderBook linked to WebSocket client for direct HFT updates");
}

void WebSocketClient::processMessages() {
    // This method is kept for API compatibility
    // In optimized version, messages are processed asynchronously
    // For now, just return - the read loop handles message processing
    return;
}




} // namespace websocket
} // namespace kubera
