#pragma once

#include "wt/session/SessionProfile.h"

#include <chrono>
#include <functional>
#include <optional>

namespace wt::session {

// Exponential reconnect backoff: initialDelay, 2x, 4x ... capped at maxDelay, with up to +-20%
// jitter so many sessions dropped together do not reconnect in lock step.
class Backoff {
public:
    // `random` returns values in [0, 1); defaults to a seeded PRNG. Tests pass a fixed function.
    explicit Backoff(ReconnectSettings settings, std::function<double()> random = {});

    // Delay before the next attempt, or nullopt once maxAttempts is exhausted (or disabled).
    std::optional<std::chrono::milliseconds> next();
    void reset() { m_attempts = 0; }
    int attempts() const { return m_attempts; }
    int maxAttempts() const { return m_settings.maxAttempts; }

private:
    ReconnectSettings m_settings;
    std::function<double()> m_random;
    int m_attempts = 0;
};

} // namespace wt::session
