#pragma once

#include <string>
#include <thread>
#include <functional>
#include <unordered_map>
#include <mutex>
#include "logging/logger.h"

namespace kubera {
namespace utils {

/**
 * @class ThreadManager
 * @brief Manages threads with CPU core pinning and priority setting
 */
class ThreadManager {
public:
    /**
     * @brief Thread priority enum
     */
    enum class ThreadPriority {
        REALTIME,       // Highest priority, for critical real-time tasks
        HIGH,           // High priority, for important tasks
        ABOVE_NORMAL,   // Above normal priority
        NORMAL,         // Normal priority
        BELOW_NORMAL,   // Below normal priority
        LOW             // Lowest priority, for background tasks
    };
    
    /**
     * @brief Thread function type
     */
    using ThreadFunction = std::function<void()>;
    
    /**
     * @brief Constructor
     * @param logger The logger instance
     */
    ThreadManager(logging::Logger& logger);
    
    /**
     * @brief Destructor
     */
    ~ThreadManager();
    
    /**
     * @brief Create a thread with the specified parameters
     * @param name The thread name
     * @param function The thread function
     * @param coreId The CPU core ID to pin the thread to (-1 for no pinning)
     * @param priority The thread priority
     * @return The created thread
     */
    std::thread createThread(
        const std::string& name,
        ThreadFunction function,
        int coreId = -1,
        ThreadPriority priority = ThreadPriority::NORMAL
    );
    
    /**
     * @brief Join a thread by name
     * @param name The thread name
     */
    void joinThread(const std::string& name);
    
    /**
     * @brief Join all threads
     */
    void joinAllThreads();
    
    /**
     * @brief Detach a thread by name
     * @param name The thread name
     */
    void detachThread(const std::string& name);
    
    /**
     * @brief Check if a thread is running
     * @param name The thread name
     * @return True if the thread is running, false otherwise
     */
    bool isThreadRunning(const std::string& name) const;
    
    /**
     * @brief Get the number of CPU cores
     * @return The number of CPU cores
     */
    size_t getNumCores() const;
    
    /**
     * @brief Pin the current thread to a CPU core
     * @param coreId The CPU core ID
     */
    void pinThreadToCore(int coreId);
    
    /**
     * @brief Set the current thread priority
     * @param priority The thread priority
     */
    void setThreadPriority(ThreadPriority priority);
    
    /**
     * @brief Set the current thread name
     * @param name The thread name
     */
    void setThreadName(const std::string& name);
    
    bool joinAllThreadsWithTimeout(int timeoutMs);

private:
    // Managed threads
    std::unordered_map<std::string, std::thread> managedThreads_;
    
    // Number of CPU cores
    size_t numCores_;
    
    // Mutex for thread map access
    mutable std::mutex mutex_;
    
    // Logger
    logging::Logger& logger_;
};

} // namespace utils
} // namespace kubera
