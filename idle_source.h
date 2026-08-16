/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_SOURCE_H
#define IDLE_SOURCE_H

#include <cstdint>
#include <string>
#include <vector>

namespace IdleDetect {

//
// The distinction between the two sentinels below is a contract with idle_detect.cpp's main loop, which
// overrides the use_event_detect config setting on IDLE_NO_GUI_SESSION and not on IDLE_ERROR. That loop now
// compares against these names rather than against the bare numbers, so the two can no longer drift apart
// there. tests/idle_aggregation_tests.cpp still pins the values against literals, because every other
// assertion in that file uses the symbolic name on both sides of the comparison and is therefore blind to a
// change in the value: the numbers are also what a reader of a debug log sees, and swapping them would
// invert the meaning of every historical log line without failing a single test.
//

//!
//! \brief Returned when a source exists but could not produce a reading. Must remain -1.
//!
constexpr int64_t IDLE_ERROR = -1;

//!
//! \brief Returned when no GUI session exists at all. Instructs the caller to defer to
//! event_detect regardless of the use_event_detect config setting. Must remain -2, and
//! therefore distinct from IDLE_ERROR.
//!
constexpr int64_t IDLE_NO_GUI_SESSION = -2;

//!
//! \brief The display server protocol a graphical endpoint speaks.
//!
enum class EndpointKind {
    WAYLAND,
    X11
};

//!
//! \brief A single reachable graphical endpoint.
//!
struct Endpoint {
    //! \brief Protocol spoken by this endpoint.
    EndpointKind m_kind;

    //! \brief Wayland socket name (e.g. "wayland-0") or canonical X display (e.g. ":1").
    std::string m_identifier;

    //!
    //! \brief Equality over both fields.
    //! \param other endpoint to compare against
    //! \return true if kind and identifier both match
    //!
    bool operator==(const Endpoint& other) const;

    //!
    //! \brief Strict weak ordering by kind, then identifier, so endpoints can key ordered
    //! containers.
    //! \param other endpoint to compare against
    //! \return true if this endpoint sorts before other
    //!
    bool operator<(const Endpoint& other) const;

    //!
    //! \brief Human-readable form for logging, e.g. "wayland:wayland-0" or "x11::1".
    //! \return string representation
    //!
    std::string ToString() const;
};

//!
//! \brief Which desktop shell owns the per-user session bus, if any.
//!
enum class ShellKind {
    NONE,
    KDE,
    GNOME
};

//!
//! \brief Abstract idle source. Each instance owns its own priority chain and resolves to
//! exactly one value. Aggregation never sees the inside of a chain.
//!
class IdleSource
{
public:
    virtual ~IdleSource() = default;

    //!
    //! \brief Resolves this source's idle time using its own internal fallback chain.
    //! \return idle seconds >= 0, or IDLE_ERROR
    //!
    virtual int64_t ResolveIdleSeconds() = 0;

    //!
    //! \brief Whether this source can still produce readings. A source that reports false is
    //! torn down and rebuilt on the next reconcile.
    //!
    //! This is deliberately pure rather than defaulted to true. The distinction it draws -- between a
    //! source that is answering badly and a source that has stopped being a source at all -- is one
    //! only the implementation can make, and a default would let a new stateful source inherit the
    //! wrong answer silently. A stateless source that opens and closes its connection inside each
    //! query cannot go stale by construction and says so explicitly.
    //!
    //! It is separate from ResolveIdleSeconds() returning IDLE_ERROR because the two mean different
    //! things and call for different responses: an error is one failed reading, excluded from the
    //! aggregate and retried on the next pass, whereas this reports that no future reading will
    //! succeed until something is rebuilt. Conflating them would either tear down a working source on
    //! a transient error, or -- the failure this exists to prevent -- keep a dead one forever and let
    //! its mere existence suppress IDLE_NO_GUI_SESSION.
    //!
    //! \return true if the source is still usable
    //!
    virtual bool IsAlive() const = 0;

    //!
    //! \brief Human-readable description for logging.
    //! \return string representation
    //!
    virtual std::string Describe() const = 0;
};

//!
//! \brief Optional mix-in for a shell source whose resolution depends on a fact only its owner
//! knows: whether any live Wayland endpoint exists.
//!
//! It is a separate interface rather than a method on IdleSource because it is not a property of
//! sources in general -- an endpoint source is told what it is by its own construction -- and it is
//! pushed rather than pulled because the pool that owns the shell source is also the thing that
//! owns the endpoint set. The alternative the design exists to eliminate is the shell source
//! reading getenv("WAYLAND_DISPLAY") for itself, which is a frozen-at-exec answer to a question
//! whose answer changes while the daemon runs.
//!
//! Declared here rather than in idle_source_pool.h so that a source implementing it need not
//! include the pool it is owned by. Implemented by ShellIdleSource; see the commentary on
//! ShellIdleSource::SetWaylandEndpointPresent() for what the flag means and why misclassifying it
//! is benign.
//!
class ShellSourceContext
{
public:
    virtual ~ShellSourceContext() = default;

    //!
    //! \brief Records whether any live Wayland endpoint source currently exists.
    //! \param present true if at least one live Wayland endpoint exists
    //!
    virtual void SetWaylandEndpointPresent(bool present) = 0;
};

//!
//! \brief Combines resolved source values into the single value returned to the main loop.
//!
//! Inhibition short-circuits to zero. Sentinels are excluded from the minimum, because
//! min(IDLE_ERROR, 300) would silently convert one source's error into a global error.
//!
//! \param resolved values returned by every live source, each >= 0 or a sentinel
//! \param inhibited whether the shell reports idle inhibition
//! \param any_source_present whether any endpoint or shell exists at all
//! \return 0 if inhibited, IDLE_NO_GUI_SESSION if nothing exists, IDLE_ERROR if sources
//!         exist but none resolved, otherwise the minimum resolved value
//!
int64_t AggregateIdleSeconds(const std::vector<int64_t>& resolved,
                             bool inhibited,
                             bool any_source_present);

} // namespace IdleDetect

#endif // IDLE_SOURCE_H
