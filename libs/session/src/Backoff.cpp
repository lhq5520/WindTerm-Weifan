#include "wt/session/Backoff.h"

#include <algorithm>
#include <random>

namespace wt::session {

Backoff::Backoff(ReconnectSettings settings, std::function<double()> random)
    : m_settings(settings), m_random(std::move(random)) {
    if (!m_random) {
        auto engine = std::make_shared<std::mt19937>(std::random_device{}());
        m_random = [engine] { return std::uniform_real_distribution<double>(0.0, 1.0)(*engine); };
    }
}

std::optional<std::chrono::milliseconds> Backoff::next() {
    if (!m_settings.enabled || m_attempts >= m_settings.maxAttempts) {
        return std::nullopt;
    }
    double delay = std::max(1, m_settings.initialDelayMs);
    for (int i = 0; i < m_attempts && delay < m_settings.maxDelayMs; ++i) {
        delay *= 2;
    }
    delay = std::min<double>(delay, std::max(m_settings.maxDelayMs, m_settings.initialDelayMs));
    const double jitter = 0.8 + 0.4 * std::clamp(m_random(), 0.0, 1.0);
    ++m_attempts;
    return std::chrono::milliseconds(static_cast<long long>(delay * jitter));
}

} // namespace wt::session
