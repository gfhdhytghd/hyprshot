#pragma once

#include <chrono>
#include <string_view>

namespace hyprcapture {

bool timingEnabled();
void traceTiming(std::string_view event, long long elapsedUs = -1);

class ScopedTiming {
  public:
    explicit ScopedTiming(std::string_view event);
    ~ScopedTiming();

    ScopedTiming(const ScopedTiming&) = delete;
    ScopedTiming& operator=(const ScopedTiming&) = delete;

  private:
    std::string_view                                m_event;
    std::chrono::steady_clock::time_point          m_started;
    bool                                           m_enabled = false;
};

} // namespace hyprcapture
