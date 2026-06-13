/*
OBS Advanced Multiview - Project logging helpers.

Implementation is intentionally tiny: a single std::atomic<bool> that
the rest of the project reads through `amv::detailed_logs_enabled()`.
Kept separate from `plugin-support.c.in` because that file is C ABI
and gets configured by CMake; the project-level helper is C++ and
deliberately not exported.
*/

#include "amv-logging.hpp"

#include <atomic>

namespace amv {

namespace {
std::atomic<bool> g_detailed_logs_enabled{false};
}

void set_detailed_logs_enabled(bool enabled)
{
	g_detailed_logs_enabled.store(enabled, std::memory_order_relaxed);
}

bool detailed_logs_enabled()
{
	return g_detailed_logs_enabled.load(std::memory_order_relaxed);
}

} // namespace amv
