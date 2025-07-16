#include "core/utils/thread_manager.h"
#include <thread>
#include <stdexcept>
#include <future>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace kubera {
namespace utils {

ThreadManager::ThreadManager(logging::Logger& logger)
    : logger_(logger) {
    // Get the number of hardware threads
    numCores_ = std::thread::hardware_concurrency();
    if (numCores_ == 0) {
        numCores_ = 4; // Default to 4 cores if detection fails
    }
    
    logger_.info("Thread manager initialized with {} cores detected", numCores_);
}

ThreadManager::~ThreadManager() {
    // Join all managed threads
    for (auto& thread : managedThreads_) {
        if (thread.second.joinable()) {
            thread.second.join();
        }
    }
}

std::thread ThreadManager::createThread(
    const std::string& name,
    ThreadFunction function,
    int coreId,
    ThreadPriority priority
) {
    try {
        // Create thread with wrapper function
        std::thread thread([this, name, function, coreId, priority]() {
            try {
                // Set thread name
                setThreadName(name);
                
                // Pin thread to core
                if (coreId >= 0) {
                    pinThreadToCore(coreId);
                }
                
                // Set thread priority
                setThreadPriority(priority);
                
                // Execute the actual function
                function();
            } catch (const std::exception& e) {
                logger_.error("Thread '{}' failed: {}", name, e.what());
            }
        });
        
        // Store thread in managed threads map
        std::lock_guard<std::mutex> lock(mutex_);
        managedThreads_[name] = std::move(thread);
        
        logger_.info("Created thread '{}' with core ID {} and priority {}", name, coreId, static_cast<int>(priority));
        
        // Return a copy of the thread (this is just a handle, the actual thread is not copied)
        return thread;
    } catch (const std::exception& e) {
        logger_.error("Failed to create thread '{}': {}", name, e.what());
        throw;
    }
}

void ThreadManager::joinThread(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = managedThreads_.find(name);
    if (it != managedThreads_.end() && it->second.joinable()) {
        it->second.join();
        logger_.info("Joined thread '{}'", name);
    } else {
        logger_.warning("Cannot join thread '{}': not found or not joinable", name);
    }
}

void ThreadManager::joinAllThreads() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& thread : managedThreads_) {
        if (thread.second.joinable()) {
            thread.second.join();
            logger_.info("Joined thread '{}'", thread.first);
        }
    }
}

void ThreadManager::detachThread(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = managedThreads_.find(name);
    if (it != managedThreads_.end() && it->second.joinable()) {
        it->second.detach();
        logger_.info("Detached thread '{}'", name);
    } else {
        logger_.warning("Cannot detach thread '{}': not found or not joinable", name);
    }
}

bool ThreadManager::isThreadRunning(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = managedThreads_.find(name);
    return (it != managedThreads_.end() && it->second.joinable());
}

size_t ThreadManager::getNumCores() const {
    return numCores_;
}

void ThreadManager::pinThreadToCore(int coreId) {
    // Ensure core ID is valid
    if (coreId < 0 || static_cast<size_t>(coreId) >= numCores_) {
        logger_.warning("Invalid core ID {}, using core 0 instead", coreId);
        coreId = 0;
    }
    
    try {
#ifdef _WIN32
        // Windows implementation
        DWORD_PTR mask = 1ULL << coreId;
        if (!SetThreadAffinityMask(GetCurrentThread(), mask)) {
            logger_.error("Failed to pin thread to core {}: error code {}", coreId, GetLastError());
        }
#elif defined(__linux__)
        // Linux implementation
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);
        
        int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (result != 0) {
            logger_.error("Failed to pin thread to core {}: error code {}", coreId, result);
        }
#else
        // macOS or other Unix-like systems don't support thread affinity
        logger_.warning("Thread pinning not supported on this platform");
#endif
        
        logger_.debug("Pinned thread to core {}", coreId);
    } catch (const std::exception& e) {
        logger_.error("Exception while pinning thread to core {}: {}", coreId, e.what());
    }
}

void ThreadManager::setThreadPriority(ThreadPriority priority) {
    try {
#ifdef _WIN32
        // Windows implementation
        int winPriority;
        switch (priority) {
            case ThreadPriority::REALTIME:
                winPriority = THREAD_PRIORITY_TIME_CRITICAL;
                break;
            case ThreadPriority::HIGH:
                winPriority = THREAD_PRIORITY_HIGHEST;
                break;
            case ThreadPriority::ABOVE_NORMAL:
                winPriority = THREAD_PRIORITY_ABOVE_NORMAL;
                break;
            case ThreadPriority::NORMAL:
                winPriority = THREAD_PRIORITY_NORMAL;
                break;
            case ThreadPriority::BELOW_NORMAL:
                winPriority = THREAD_PRIORITY_BELOW_NORMAL;
                break;
            case ThreadPriority::LOW:
                winPriority = THREAD_PRIORITY_LOWEST;
                break;
            default:
                winPriority = THREAD_PRIORITY_NORMAL;
        }
        
        if (!SetThreadPriority(GetCurrentThread(), winPriority)) {
            logger_.error("Failed to set thread priority: error code {}", GetLastError());
        }
#else
        // Linux/Unix implementation
        int policy;
        struct sched_param param;
        
        pthread_getschedparam(pthread_self(), &policy, &param);
        
        switch (priority) {
            case ThreadPriority::REALTIME:
                policy = SCHED_FIFO;
                param.sched_priority = 99;
                break;
            case ThreadPriority::HIGH:
                policy = SCHED_FIFO;
                param.sched_priority = 80;
                break;
            case ThreadPriority::ABOVE_NORMAL:
                policy = SCHED_FIFO;
                param.sched_priority = 60;
                break;
            case ThreadPriority::NORMAL:
                policy = SCHED_OTHER;
                param.sched_priority = 0;
                break;
            case ThreadPriority::BELOW_NORMAL:
                policy = SCHED_OTHER;
                param.sched_priority = 0;
                break;
            case ThreadPriority::LOW:
                policy = SCHED_OTHER;
                param.sched_priority = 0;
                break;
            default:
                policy = SCHED_OTHER;
                param.sched_priority = 0;
        }
        
        int result = pthread_setschedparam(pthread_self(), policy, &param);
        if (result != 0) {
            logger_.error("Failed to set thread priority: error code {}", result);
        }
#endif
        
        logger_.debug("Set thread priority to {}", static_cast<int>(priority));
    } catch (const std::exception& e) {
        logger_.error("Exception while setting thread priority: {}", e.what());
    }
}

void ThreadManager::setThreadName(const std::string& name) {
    try {
#ifdef _WIN32
        // Windows implementation (requires Windows 10 1607 or later)
        wchar_t wname[256];
        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname, 256);
        SetThreadDescription(GetCurrentThread(), wname);
#elif defined(__APPLE__)
        // macOS implementation (only accepts 16 characters)
        std::string truncatedName = name.substr(0, 15);
        pthread_setname_np(truncatedName.c_str());
#elif defined(__linux__)
        // Linux implementation
        pthread_setname_np(pthread_self(), name.c_str());
#endif
        
        logger_.debug("Set thread name to '{}'", name);
    } catch (const std::exception& e) {
        logger_.error("Exception while setting thread name: {}", e.what());
    }
}

bool ThreadManager::joinAllThreadsWithTimeout(int timeoutMs) {
    bool allJoined = true;
    
    // Use the managedThreads_ map to access threads
    for (auto& threadPair : managedThreads_) {
        std::thread& t = threadPair.second;
        if (t.joinable()) {
            // Try joining with timeout
            std::future<void> future = std::async(std::launch::async, [&t]() {
                t.join();
            });
            
            if (future.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::timeout) {
                logger_.warning("Thread '{}' did not join within timeout", threadPair.first);
                allJoined = false;
                // Don't detach - keep trying to join in destructor
            }
        }
    }
    
    return allJoined;
}
} // namespace utils
} // namespace kubera
