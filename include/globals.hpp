#pragma once
#include <atomic>
#include <mutex>

extern std::atomic<bool> g_stop;
extern std::mutex g_log_mu;