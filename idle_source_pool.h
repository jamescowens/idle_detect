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
//! \brief Reconcile ticks to skip after a candidate endpoint's first validation failure. Each
//! further consecutive failure doubles the interval, up to ENDPOINT_RETRY_BACKOFF_MAX_TICKS.
//!
constexpr int ENDPOINT_RETRY_BACKOFF_INITIAL_TICKS = 1;

//!
//! \brief Ceiling on the retry interval for a repeatedly failing candidate, in reconcile ticks.
//!
//! A permanently-bad candidate therefore costs one validation attempt per 64 ticks forever rather
//! than one per tick. That is the difference between a bounded background cost and the measured
//! failure this constant exists to prevent: a stale $XDG_RUNTIME_DIR/wayland-9 socket with no
//! listener produced 87 journal lines in 8 seconds with debug logging off, roughly 604,000 lines a
//! day. GNOME is the ordinary case, not an exotic one -- it does not advertise ext_idle_notifier_v1
//! at all, so every Wayland candidate in a GNOME session is a permanent validation failure.
//!
constexpr int ENDPOINT_RETRY_BACKOFF_MAX_TICKS = 64;

//!
//! \brief Consecutive IDLE_ERROR resolutions before an endpoint's source is torn down and its endpoint
//! treated as newly-seen.
//!
//! This is the counterpart to IdleSource::IsAlive(), and the two are kept because they catch disjoint
//! populations. IsAlive() catches sources that KNOW they are dead -- a Wayland monitor whose thread
//! exited on a hangup can say so. This catches sources that CANNOT tell, which is every stateless one:
//! X11IdleSource opens and closes its connection inside each query, so it has no state in which to
//! notice that the display it queries no longer exists.
//!
//! The failure it was written against was measured. SIGKILLing an X server leaves its socket in
//! /tmp/.X11-unix, so discovery goes on offering the candidate, the source is retained on the strength
//! of the key alone, and it resolves IDLE_ERROR forever -- 14 consecutive errors over the remainder of a
//! 20 second run on :47, with no teardown. Worse than the wasted queries is what its mere existence
//! does: it counts toward any_source_present, so IDLE_NO_GUI_SESSION can never fire, and a session with
//! no readable idle source at all is never handed to event_detect.
//!
//! Three rather than one, because a single failed reading is an ordinary transient -- an X server busy
//! for a moment, a compositor mid-reconfiguration -- and the response to it is to try again on the next
//! tick, not to drop the endpoint to the bottom of the retry ladder. Three consecutive failures, at one
//! resolution per main loop iteration, is a source that has stopped working rather than one that
//! stuttered. The teardown is cheap to get wrong in this direction anyway: the endpoint is immediately
//! newly-seen, so a display that is answering again is rebuilt on the same tick.
//!
constexpr int DEAD_SOURCE_ERROR_THRESHOLD = 3;

//!
//! \brief Consecutive reconciles reporting ShellKind::NONE before a shell that was present is acted
//! on as gone.
//!
//! The shell kind is re-derived from live D-Bus probes on every tick with nothing to smooth it, so a
//! single failed NameHasOwner -- a bus hiccup, a timeout under load, a session bus restart -- reads
//! as "there is no shell". In a shell-only session, KDE on X11 or GNOME with no validated endpoint,
//! the shell is the ONLY source, so acting on that one observation drops the pool to no sources at
//! all, reports IDLE_NO_GUI_SESSION, and flips the daemon's source of truth to event_detect for a
//! tick over a probe that was answering again immediately afterwards.
//!
//! Only the DISAPPEARING direction is debounced. A shell appearing is acted on at once, because that
//! direction is not harmful: the worst case is a source built one tick early, and it is validated by
//! its factory before it is used. Endpoints are not debounced at all -- their discovery is a
//! filesystem scan plus a validating connect, neither of which has this failure mode, and delaying
//! their eviction would keep a source alive against a compositor that is already gone.
//!
constexpr int SHELL_ABSENCE_OBSERVATIONS_REQUIRED = 3;

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
//! A nullptr return is never cached as a verdict. Discovery re-offers the candidate and the factory
//! is called again, so a compositor that is still starting up is picked up on a later tick rather
//! than being locked out for the life of the process. What IS remembered is how many times in a row
//! the candidate has failed, which throttles how often "again" is: see the backoff commentary on
//! Reconcile().
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
//! that disappeared are torn down and evicted, and endpoints whose source has stopped being able to
//! produce readings are torn down and rebuilt. Eviction is immediate rather than deferred, because
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
    //! Sources for endpoints that are still present AND still working are left strictly alone.
    //! Rebuilding them on unrelated churn would drop and re-establish a working compositor connection
    //! every time some other endpoint appeared or went away. Two things are not unrelated churn, and a
    //! source that exhibits either is torn down and its endpoint treated as newly-seen, because the
    //! candidate key surviving says only that the socket is still there, not that anything is still
    //! answering on it:
    //!
    //!   - The source reports IdleSource::IsAlive() false, i.e. it knows it is dead.
    //!   - The source has resolved IDLE_ERROR DEAD_SOURCE_ERROR_THRESHOLD times in a row, i.e. it does
    //!     not know it is dead but has stopped behaving as though it is alive.
    //!
    //! See DEAD_SOURCE_ERROR_THRESHOLD for why both are needed rather than either alone.
    //!
    //! A candidate that fails validation is retried, but not on every tick. Consecutive failures are
    //! counted per endpoint and the retry is deferred by an exponentially growing interval -- one
    //! tick, then two, four, eight, capped at ENDPOINT_RETRY_BACKOFF_MAX_TICKS. The counter is reset
    //! the moment validation succeeds or the endpoint leaves the candidate set, so a compositor
    //! restart or a socket that is replugged recovers on the tick after it reappears rather than
    //! waiting out a backoff it did not earn.
    //!
    //! THE INTERVAL IS COUNTED IN RECONCILE TICKS RATHER THAN IN SECONDS, and that is a deliberate
    //! consequence of this file being dependency-free: the pool has no clock and is not going to
    //! grow one, because taking a time source would mean taking either a global or another injected
    //! callable purely to schedule retries. Ticks are what the pool can count without help, and they
    //! are a sound unit here because the caller reconciles on a fixed interval, so a tick count is a
    //! wall-clock interval scaled by that period. The property it lacks is that a caller which
    //! reconciles on an event rather than a timer would back off in events rather than seconds; that
    //! is acceptable, since the backoff exists to bound work per reconcile, and the number of
    //! reconciles is exactly what it bounds it against.
    //!
    //! The shell argument is an OBSERVATION rather than a verdict. A shell that appears is acted on
    //! immediately, while a shell that has gone missing must be missing for
    //! SHELL_ABSENCE_OBSERVATIONS_REQUIRED consecutive reconciles before its source is dropped; see
    //! that constant for why the two directions differ.
    //!
    //! \param endpoints Candidate endpoints from discovery.
    //! \param shell Shell kind observed by the caller on this tick.
    //!
    void Reconcile(const std::set<Endpoint>& endpoints, ShellKind shell);

    //!
    //! \brief Resolves every live source and aggregates per the sentinel contract.
    //!
    //! Also maintains the per-endpoint consecutive error counts that Reconcile() acts on. They are
    //! updated here rather than there because this is the only place a source is actually resolved; the
    //! two halves are split across the two calls so that a source is never torn down in the middle of the
    //! pass that is reading it.
    //!
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
    //! \brief Retry state for a candidate endpoint whose validation has failed at least once.
    //!
    //! Only failing candidates have an entry. A candidate that validates, and a candidate that has
    //! left the set, both have their entry erased, which is what makes recovery immediate.
    //!
    struct EndpointBackoff {
        //! \brief Consecutive validation failures, counting the one that created this entry.
        int m_consecutive_failures = 0;

        //!
        //! \brief Interval applied after the most recent failure, in reconcile ticks. Retained so a
        //! change in it can be logged, and so the next interval is a doubling of a real value rather
        //! than a shift by a failure count that grows without bound.
        //!
        int m_interval_ticks = 0;

        //! \brief Reconcile ticks still to be skipped before the factory is called again.
        int m_ticks_until_retry = 0;
    };

    //!
    //! \brief Whether a failing candidate's retry is still deferred, consuming one tick of its
    //! backoff if so. Called with mtx_pool held.
    //!
    //! The tick is consumed here, in the one place that asks the question, rather than in a separate
    //! pass over the map: an endpoint that is no longer a candidate has no entry to decrement, so
    //! coupling the countdown to the candidate's presence is what keeps a disappeared endpoint from
    //! silently ageing out of a backoff it is not serving.
    //!
    //! \param endpoint Candidate being considered.
    //! \return true if the factory must not be called for this endpoint on this tick.
    //!
    bool BackoffDefersCandidate(const Endpoint& endpoint);

    //!
    //! \brief Records a validation failure and schedules the next attempt. Called with mtx_pool
    //! held.
    //!
    //! Logging level is decided here rather than by the caller, because the level is a function of
    //! the backoff state: the first failure and each subsequent growth of the interval are reported
    //! at normal level, and every failure after the interval has reached its ceiling is debug only.
    //! A permanently-bad candidate therefore produces a small, bounded number of normal-level lines
    //! over the life of the process instead of one per tick forever.
    //!
    //! \param endpoint Candidate that failed validation.
    //!
    void RecordCandidateFailure(const Endpoint& endpoint);

    //!
    //! \brief Turns a shell observation into the shell kind to act on, debouncing disappearance.
    //! Called with mtx_pool held.
    //!
    //! Counting observations rather than elapsed time, for the same reason the endpoint backoff counts
    //! ticks: this file is dependency-free and has no clock. The unit is right either way, because what
    //! is being defended against is a probe failing, and probes happen once per reconcile.
    //!
    //! \param observed Shell kind the caller detected on this tick.
    //! \return Shell kind to reconcile against, which is the previously acted-on kind while an absence
    //!         is still being confirmed.
    //!
    ShellKind DebounceShellObservation(ShellKind observed);

    //!
    //! \brief Pushes the context only the pool knows onto the shell source, if that source wants
    //! it. Called with mtx_pool held.
    //!
    //! The context in question is whether this is a WAYLAND SESSION, and the pool answers it from the
    //! candidate set rather than from its live sources. Those are two different questions -- "a Wayland
    //! session exists" and "we have a working Wayland idle source" -- and only the first one is the
    //! shell's. See the commentary in the definition for the failure that conflating them produced.
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

    //!
    //! \brief Retry state for candidate endpoints that are currently failing validation.
    //!
    //! Disjoint from m_endpoint_sources by construction: an endpoint with a live source has no
    //! backoff entry, and an endpoint with a backoff entry has no source.
    //!
    std::map<Endpoint, EndpointBackoff> m_endpoint_backoff;

    //!
    //! \brief Consecutive IDLE_ERROR resolutions per endpoint that currently has a source.
    //!
    //! The complement of m_endpoint_backoff, which tracks endpoints that have NO source. Only endpoints
    //! currently erroring have an entry: a successful resolution erases it, and so does the destruction
    //! of the source, so a rebuilt source always starts from zero rather than inheriting the count that
    //! condemned its predecessor.
    //!
    //! A separate map rather than a field beside the source, so that the source container stays a plain
    //! map of owned pointers and this bookkeeping is visibly the pool's rather than the source's. It has
    //! to be the pool's: the whole point is to catch sources that cannot tell they have failed.
    //!
    std::map<Endpoint, int> m_endpoint_consecutive_errors;

    //! \brief The single shell source, null when no shell is present or its factory declined.
    std::unique_ptr<IdleSource> m_shell_source;

    //!
    //! \brief Currently detected shell kind.
    //!
    //! Tracked independently of m_shell_source, because inhibition is evaluated from the kind and
    //! must keep working when the shell has no idle value and its factory therefore returned
    //! nullptr.
    //!
    //! This is the kind most recently ACTED ON, not the most recent observation. While an absence is
    //! being confirmed the two differ, and this one is what the pool reconciles and evaluates
    //! inhibition against.
    //!
    ShellKind m_shell_kind;

    //!
    //! \brief Consecutive reconciles that have observed no shell while one was being tracked.
    //!
    //! Reset by any observation of a shell, and by acting on the absence, so it only ever counts an
    //! unbroken run.
    //!
    int m_shell_absence_observations;

    //!
    //! \brief Whether the most recent Reconcile() was offered any Wayland endpoint CANDIDATE.
    //!
    //! Deliberately not "whether a Wayland source is live". A Wayland socket in $XDG_RUNTIME_DIR means
    //! the user's session is a Wayland session, whether or not this daemon can read an idle time out of
    //! the compositor behind it, and it is the session's protocol that the shell source needs to know.
    //! Held as state rather than recomputed on demand because the shell source is updated at the end of
    //! Reconcile(), by which point the candidate set is out of scope.
    //!
    bool m_wayland_candidate_present;
};

} // namespace IdleDetect

#endif // IDLE_SOURCE_POOL_H
