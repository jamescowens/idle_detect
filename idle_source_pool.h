/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_SOURCE_POOL_H
#define IDLE_SOURCE_POOL_H

#include <idle_source.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

//
// This file, like idle_source.* and session_discovery.*, is deliberately free of D-Bus, X11 and
// Wayland dependencies, so that it links into idle_detect_tests -- which links none of those
// libraries. Every piece of system contact the pool needs arrives through the injected factories
// below, whose implementations live in idle_sources_system.* and are supplied by idle_detect.cpp.
//
// That injection is the whole point of the split. The pool's logic is the part of this design most
// likely to break in a way no compiler catches: an endpoint source silently torn down and rebuilt
// on unrelated churn, a validation failure cached so the endpoint never comes back, a shell value
// leaking into an endpoint's reading. Put in the system file, none of it could be tested at all.
// Put here, all of it is.
//

namespace IdleDetect {

//!
//! \brief Creates a started, validated source for an endpoint, or nullptr if the candidate is not
//! usable.
//!
//! Validation is the factory's job, not the pool's, because what "validated" means is
//! protocol-specific: a Wayland candidate is validated by connecting to the socket and binding
//! ext_idle_notifier_v1, an X11 candidate by taking one reading. The pool only distinguishes a
//! source from a nullptr. Any tuning the construction needs -- notification timeouts, retry budgets
//! -- is captured by the factory rather than threaded through Reconcile().
//!
//! A nullptr return is never cached. Discovery re-offers the candidate on the next reconcile tick
//! and the factory is called again, so a compositor that is still starting up is picked up on a
//! later tick rather than being locked out for the life of the process.
//!
using EndpointSourceFactory = std::function<std::unique_ptr<IdleSource>(const Endpoint&)>;

//!
//! \brief Creates the single shell source for a shell kind, or nullptr for ShellKind::NONE.
//!
//! The factory is also responsible for stamping any bus-derived context onto the source it returns
//! -- in the production implementation, whether mutter's IdleMonitor is on the bus, which decides
//! whether a GNOME shell has an idle value or contributes inhibition only. The pool holds no bus
//! connection and cannot answer that question. It does supply the one piece of context it alone
//! knows, the live Wayland endpoint set, through ShellSourceContext.
//!
using ShellSourceFactory = std::function<std::unique_ptr<IdleSource>(ShellKind)>;

//!
//! \brief Reports whether the shell currently inhibits idling.
//!
//! Separate from the shell source rather than folded into it, because inhibition is a global
//! override rather than a reading: it applies ahead of every source, including endpoint sources
//! the shell knows nothing about, and it still applies when the shell has no idle value to give
//! or when its source could not be constructed at all.
//!
using InhibitionQuery = std::function<bool(ShellKind)>;

//!
//! \brief The IdleSourcePool class owns all live idle sources and produces the single aggregate
//! idle value the main loop consumes.
//!
//! Endpoint sources are reconciled against discovery: newly-seen endpoints are started, endpoints
//! that disappeared are torn down and evicted. Eviction is immediate rather than deferred, because
//! a source left reporting against a dead compositor would pin the aggregate to "active"
//! indefinitely.
//!
//! The shell is not an endpoint and is held separately. With N endpoints and one shell, at most one
//! endpoint is the shell's screen and there is no reliable way to determine which, so fusing the
//! shell into an endpoint's chain would make every endpoint report the shell's idle time and
//! silently discard a second endpoint's real activity -- a console session idle for twenty minutes
//! next to a VNC session active three seconds ago would report twenty minutes.
//!
//! \warning The injected callables are invoked with mtx_pool held. They must not call back into the
//! pool.
//!
class IdleSourcePool
{
public:
    //!
    //! \brief Constructor. Takes no action until the first Reconcile().
    //! \param endpoint_factory Creates and validates endpoint sources.
    //! \param shell_factory Creates the shell source.
    //! \param inhibition_query Evaluates the inhibition override.
    //!
    IdleSourcePool(EndpointSourceFactory endpoint_factory,
                   ShellSourceFactory shell_factory,
                   InhibitionQuery inhibition_query);

    //! \brief Destructor. Tears down every source.
    ~IdleSourcePool();

    //! \brief Deleted copy constructor and assignment operator. This owns source lifetimes.
    IdleSourcePool(const IdleSourcePool&) = delete;
    IdleSourcePool& operator=(const IdleSourcePool&) = delete;

    //!
    //! \brief Brings the live source set in line with discovery. Starts newly-seen endpoints,
    //! evicts endpoints that disappeared, and updates the shell source.
    //!
    //! Sources for endpoints that are still present are left strictly alone. Rebuilding them on
    //! unrelated churn would drop and re-establish a working compositor connection every time some
    //! other endpoint appeared or went away.
    //!
    //! \param endpoints Candidate endpoints from discovery.
    //! \param shell Currently detected shell kind.
    //!
    void Reconcile(const std::set<Endpoint>& endpoints, ShellKind shell);

    //!
    //! \brief Resolves every live source and aggregates per the sentinel contract.
    //! \return 0 if inhibited, IDLE_NO_GUI_SESSION if no source exists at all, IDLE_ERROR if
    //!         sources exist but none resolved, otherwise the minimum resolved value.
    //!
    int64_t GetIdleSeconds();

    //!
    //! \brief Tears down every source. Idempotent, and leaves the pool reusable: a later
    //! Reconcile() repopulates it.
    //!
    void Shutdown();

    //!
    //! \brief Number of live endpoint sources. Diagnostics for logging and tests.
    //! \return count of endpoint sources
    //!
    size_t EndpointSourceCount() const;

    //!
    //! \brief Whether a shell source is live. Diagnostics for logging and tests.
    //! \return true if a shell source exists
    //!
    bool HasShellSource() const;

private:
    //!
    //! \brief Pushes the context only the pool knows onto the shell source, if that source wants
    //! it. Called with mtx_pool held.
    //!
    void UpdateShellSourceContext();

    //! \brief Guards the source containers against concurrent reconcile and query.
    mutable std::mutex mtx_pool;

    //! \brief Creates and validates endpoint sources.
    EndpointSourceFactory m_endpoint_factory;

    //! \brief Creates the shell source.
    ShellSourceFactory m_shell_factory;

    //! \brief Evaluates the inhibition override.
    InhibitionQuery m_inhibition_query;

    //! \brief One source per live endpoint, keyed by endpoint.
    std::map<Endpoint, std::unique_ptr<IdleSource>> m_endpoint_sources;

    //! \brief The single shell source, null when no shell is present or its factory declined.
    std::unique_ptr<IdleSource> m_shell_source;

    //!
    //! \brief Currently detected shell kind.
    //!
    //! Tracked independently of m_shell_source, because inhibition is evaluated from the kind and
    //! must keep working when the shell has no idle value and its factory therefore returned
    //! nullptr.
    //!
    ShellKind m_shell_kind;
};

} // namespace IdleDetect

#endif // IDLE_SOURCE_POOL_H
