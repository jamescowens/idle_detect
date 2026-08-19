/*
 * Copyright (C) 2025-2026 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <idle_source.h>

#include <tuple>

namespace IdleDetect {

bool Endpoint::operator==(const Endpoint& other) const
{
    return m_kind == other.m_kind && m_identifier == other.m_identifier;
}

bool Endpoint::operator<(const Endpoint& other) const
{
    return std::tie(m_kind, m_identifier) < std::tie(other.m_kind, other.m_identifier);
}

std::string Endpoint::ToString() const
{
    const std::string prefix = (m_kind == EndpointKind::WAYLAND) ? "wayland:" : "x11:";

    return prefix + m_identifier;
}

int64_t AggregateIdleSeconds(const std::vector<int64_t>& resolved,
                             bool inhibited,
                             bool any_source_present)
{
    // Inhibition means "do not let this machine go idle" and wins over everything, including
    // sources that failed to resolve.
    if (inhibited) {
        return 0;
    }

    if (!any_source_present) {
        return IDLE_NO_GUI_SESSION;
    }

    bool found = false;
    int64_t minimum = 0;

    for (const int64_t value : resolved) {
        // Sentinels are negative and must never participate in the minimum.
        if (value < 0) {
            continue;
        }

        if (!found || value < minimum) {
            minimum = value;
            found = true;
        }
    }

    return found ? minimum : IDLE_ERROR;
}

} // namespace IdleDetect
