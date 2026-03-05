#include "globals.hpp"

std::atomic<bool> g_stop{false};
std::mutex g_log_mu;