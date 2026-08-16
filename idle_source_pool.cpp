/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <idle_source_pool.h>
#include <util.h>

#include <utility>
#include <vector>

namespace IdleDetect {

IdleSourcePool::IdleSourcePool(EndpointSourceFactory endpoint_factory,
                               ShellSourceFactory shell_factory,
                               InhibitionQuery inhibition_query)
    : m_endpoint_factory(std::move(endpoint_factory))
    , m_shell_factory(std::move(shell_factory))
    , m_inhibition_query(std::move(inhibition_query))
    , m_shell_kind(ShellKind::NONE)
    , m_shell_absence_observations(0)
    , m_wayland_candidate_present(false)
{}

IdleSourcePool::~IdleSourcePool()
{
    Shutdown();
}

void IdleSourcePool::Reconcile(const std::set<Endpoint>& endpoints, ShellKind shell)
{
    std::unique_lock<std::mutex> lock(mtx_pool);

    // --- Evict sources whose endpoint disappeared. ---
    //
    // Erasing the map entry destroys the source, and destruction is what stops it. This is done
    // before anything is added so that a tick which replaces one endpoint with another releases the
    // old one's resources first.
    for (auto iter = m_endpoint_sources.begin(); iter != m_endpoint_sources.end();) {
        if (endpoints.count(iter->first) == 0) {
            normal_log("INFO: %s: Evicting idle source %s.", __func__, iter->second->Describe().c_str());

            iter = m_endpoint_sources.erase(iter);
        } else {
            ++iter;
        }
    }

    // --- Drop retry state for endpoints that are no longer candidates. ---
    //
    // Leaving the candidate set is the pool's only evidence that the thing behind the endpoint has
    // changed, so it is what resets the backoff ladder. A socket that is replugged, or a compositor
    // that is restarted, therefore gets a full-speed attempt on the tick it reappears rather than
    // serving out an interval earned by whatever was there before.
    for (auto iter = m_endpoint_backoff.begin(); iter != m_endpoint_backoff.end();) {
        if (endpoints.count(iter->first) == 0) {
            debug_log("INFO: %s: Candidate endpoint %s is gone; clearing its validation backoff.",
                      __func__,
                      iter->first.ToString().c_str());

            iter = m_endpoint_backoff.erase(iter);
        } else {
            ++iter;
        }
    }

    // --- Tear down retained sources that can no longer produce readings. ---
    //
    // The endpoint key still being in the candidate set is NOT proof that the source behind it works.
    // A Wayland socket outlives the compositor's ability to serve it: the monitor thread exits on a
    // hangup or on the removal of a global it depends on, while the socket that produced the candidate
    // sits in $XDG_RUNTIME_DIR exactly as before. Retaining on key alone left such a source in place
    // for the life of the process, which is the frozen-value bug this design fixed inside the monitor,
    // reintroduced one layer up: the source could only ever return IDLE_ERROR, and its mere existence
    // kept any_source_present true, so IDLE_NO_GUI_SESSION could never fire and the daemon neither read
    // the compositor nor fell back to event_detect.
    //
    // Destroying it here rather than marking it puts the endpoint back in the newly-seen state below,
    // so the rebuild goes through validation and the backoff like any other candidate. That is what
    // keeps a compositor that is genuinely gone from being reconnected on every tick.
    for (auto iter = m_endpoint_sources.begin(); iter != m_endpoint_sources.end();) {
        if (!iter->second->IsAlive()) {
            normal_log("INFO: %s: Idle source %s is no longer alive. Tearing it down; it will be rebuilt "
                       "if its endpoint still validates.",
                       __func__,
                       iter->second->Describe().c_str());

            iter = m_endpoint_sources.erase(iter);
        } else {
            ++iter;
        }
    }

    // --- Add sources for newly-seen endpoints. ---
    //
    // Endpoints already holding a live source are skipped entirely rather than rebuilt, because a live
    // source may own a compositor connection and a thread that took real work to establish.
    for (const Endpoint& endpoint : endpoints) {
        if (m_endpoint_sources.count(endpoint) > 0) {
            continue;
        }

        // A candidate that has been failing is not offered to the factory again until its backoff
        // has elapsed. This is what bounds the cost of a permanently-bad candidate, which is the
        // ordinary case rather than the exotic one: a GNOME session advertises no
        // ext_idle_notifier_v1, so its Wayland candidate can never validate, and without this it
        // would be rebuilt, restarted and re-logged on every tick for the life of the process.
        if (BackoffDefersCandidate(endpoint)) {
            continue;
        }

        std::unique_ptr<IdleSource> source = m_endpoint_factory ? m_endpoint_factory(endpoint) : nullptr;

        // A null return means the candidate did not validate. Discovery is deliberately
        // over-inclusive, so this is an ordinary outcome and not an error. The failure is not cached
        // as a verdict -- the candidate is offered again on a later tick, which is how a compositor
        // or X server that is still starting up gets picked up -- but it does advance the backoff
        // that decides which later tick that is.
        if (!source) {
            RecordCandidateFailure(endpoint);

            continue;
        }

        // Success clears the ladder outright, so an endpoint that finally comes up after a long
        // backoff is not carrying an interval into whatever happens to it next.
        m_endpoint_backoff.erase(endpoint);

        normal_log("INFO: %s: Added idle source %s.", __func__, source->Describe().c_str());

        m_endpoint_sources.emplace(endpoint, std::move(source));
    }

    // --- Reconcile the shell source. ---
    //
    // The observation is debounced first. The caller re-derives the shell from live bus probes on
    // every tick, so one failed NameHasOwner would otherwise drop a perfectly good shell, and in a
    // shell-only session the shell is the only source there is.
    const ShellKind effective_shell = DebounceShellObservation(shell);

    // The kind is recorded whatever happens to the source, because inhibition is evaluated from the
    // kind. A GNOME shell without mutter has no idle value and its factory may well decline to
    // build a source for it, but it still inhibits.
    if (effective_shell == ShellKind::NONE) {
        if (m_shell_source) {
            normal_log("INFO: %s: Desktop shell went away; dropping shell idle source.", __func__);

            m_shell_source.reset();
        }
    } else if (!m_shell_source || effective_shell != m_shell_kind) {
        // Replacing rather than updating on a kind change: the shell source is reached through the
        // abstract IdleSource interface, which has no SetKind(), and a source built for the previous
        // shell would go on querying a bus name that nothing owns any more.
        //
        // The !m_shell_source arm also covers the retry case. A factory that declined last tick is
        // called again on this one, on the same no-caching rule the endpoint factory follows.
        //
        // The old source is released before the new one is built rather than being dropped by the
        // assignment afterwards, so that two shell sources are never alive at once. There is one
        // shell, and a replacement that briefly overlaps its predecessor would have both of them
        // holding whatever the shell source holds.
        m_shell_source.reset();

        m_shell_source = m_shell_factory ? m_shell_factory(effective_shell) : nullptr;

        if (m_shell_source) {
            normal_log("INFO: %s: Added shell idle source %s.", __func__, m_shell_source->Describe().c_str());
        } else {
            debug_log("INFO: %s: No shell idle source was created for the detected shell. Inhibition "
                      "still applies, and creation will be retried on the next reconcile.",
                      __func__);
        }
    }

    // The debounced kind, not the raw observation. Inhibition is evaluated from this, so recording
    // the observation here would drop inhibition on the very tick a probe hiccuped -- resuming the
    // compute the user explicitly asked to hold off -- while the source it belongs to was retained.
    m_shell_kind = effective_shell;

    // --- Record whether this is a Wayland SESSION, which is not the same question as whether a
    // --- Wayland source works.
    //
    // "A Wayland session exists" is answered by the CANDIDATE set: a wayland-* socket in
    // $XDG_RUNTIME_DIR, or a WAYLAND_DISPLAY the session declares, means a Wayland compositor is what
    // this user is sitting in front of. "We have a working Wayland idle source" is answered by
    // m_endpoint_sources, and it is a strictly narrower thing: it additionally requires the compositor
    // to advertise ext_idle_notifier_v1, the connection to be up right now, and the candidate not to be
    // serving out a validation backoff.
    //
    // Only the first question is the shell's. See UpdateShellSourceContext() for what happens when the
    // two are conflated, which is the defect this line fixes.
    m_wayland_candidate_present = false;

    for (const Endpoint& endpoint : endpoints) {
        if (endpoint.m_kind == EndpointKind::WAYLAND) {
            m_wayland_candidate_present = true;
            break;
        }
    }

    UpdateShellSourceContext();
}

int64_t IdleSourcePool::GetIdleSeconds()
{
    std::unique_lock<std::mutex> lock(mtx_pool);

    // Inhibition is evaluated first and independently of the sources. It is a global override rather
    // than a reading, it applies even when no source resolved, and it does not depend on the shell
    // having produced a source at all.
    const bool inhibited = m_inhibition_query ? m_inhibition_query(m_shell_kind) : false;

    // A shell counts as a source in its own right. Requiring an endpoint here would report
    // IDLE_NO_GUI_SESSION for a session whose shell is answering perfectly well, handing it to
    // event_detect for no reason.
    const bool any_source_present = !m_endpoint_sources.empty() || m_shell_source != nullptr;

    std::vector<int64_t> resolved;
    resolved.reserve(m_endpoint_sources.size() + 1);

    // Each source resolves through its own chain and contributes exactly one value. The values stay
    // separate all the way into AggregateIdleSeconds(): the shell is never folded into an endpoint's
    // reading, because with N endpoints and one shell there is no way to know which endpoint the
    // shell is describing, and guessing wrong discards a second endpoint's real activity.
    for (auto& entry : m_endpoint_sources) {
        const int64_t value = entry.second->ResolveIdleSeconds();

        debug_log("INFO: %s: Source %s resolved to %lld.", __func__, entry.second->Describe().c_str(), value);

        resolved.push_back(value);
    }

    if (m_shell_source) {
        const int64_t value = m_shell_source->ResolveIdleSeconds();

        debug_log("INFO: %s: Source %s resolved to %lld.", __func__, m_shell_source->Describe().c_str(), value);

        resolved.push_back(value);
    }

    return AggregateIdleSeconds(resolved, inhibited, any_source_present);
}

void IdleSourcePool::Shutdown()
{
    std::unique_lock<std::mutex> lock(mtx_pool);

    m_endpoint_sources.clear();
    m_shell_source.reset();

    // The retry ladders go with the sources. A pool that has been shut down and repopulated is
    // starting over, and holding a candidate at a 64-tick interval across that would be the one
    // thing this state must never do: outlive the situation that produced it.
    m_endpoint_backoff.clear();

    // Clearing the kind too, so that a pool which has been shut down cannot go on reporting
    // inhibition for a shell it is no longer tracking. The absence run goes with it: there is now no
    // shell whose disappearance could be part-way confirmed.
    m_shell_kind = ShellKind::NONE;
    m_shell_absence_observations = 0;

    // A shut-down pool has been offered no candidates, so it knows of no Wayland session. The next
    // Reconcile() re-derives this from the candidate set it is handed, before any shell source can read
    // it.
    m_wayland_candidate_present = false;
}

size_t IdleSourcePool::EndpointSourceCount() const
{
    std::unique_lock<std::mutex> lock(mtx_pool);

    return m_endpoint_sources.size();
}

bool IdleSourcePool::HasShellSource() const
{
    std::unique_lock<std::mutex> lock(mtx_pool);

    return m_shell_source != nullptr;
}

bool IdleSourcePool::BackoffDefersCandidate(const Endpoint& endpoint)
{
    auto iter = m_endpoint_backoff.find(endpoint);

    if (iter == m_endpoint_backoff.end()) {
        return false;
    }

    if (iter->second.m_ticks_until_retry <= 0) {
        return false;
    }

    --iter->second.m_ticks_until_retry;

    debug_log("INFO: %s: Candidate endpoint %s is backed off after %d consecutive validation "
              "failure(s); %d more reconcile tick(s) before it is offered again.",
              __func__,
              endpoint.ToString().c_str(),
              iter->second.m_consecutive_failures,
              iter->second.m_ticks_until_retry);

    return true;
}

void IdleSourcePool::RecordCandidateFailure(const Endpoint& endpoint)
{
    EndpointBackoff& backoff = m_endpoint_backoff[endpoint];

    const int previous_interval_ticks = backoff.m_interval_ticks;

    ++backoff.m_consecutive_failures;

    // Doubling the previous interval rather than shifting by the failure count. The failure count of
    // a candidate that is permanently bad grows for as long as the daemon runs, and a shift by it
    // would be undefined long before the value it produced was clamped.
    int interval_ticks = (previous_interval_ticks == 0)
            ? ENDPOINT_RETRY_BACKOFF_INITIAL_TICKS
            : previous_interval_ticks * 2;

    if (interval_ticks > ENDPOINT_RETRY_BACKOFF_MAX_TICKS) {
        interval_ticks = ENDPOINT_RETRY_BACKOFF_MAX_TICKS;
    }

    backoff.m_interval_ticks = interval_ticks;
    backoff.m_ticks_until_retry = interval_ticks;

    // One line, at one level, per failure. The first failure is worth telling the operator about,
    // and so is each step of the retreat, because those are the transitions that describe what the
    // daemon has decided about this candidate. Once the interval has reached its ceiling nothing
    // further is being decided, and the remaining failures are debug material.
    const char* message = "INFO: %s: Candidate endpoint %s did not validate (%d consecutive "
                          "failure(s)). Retrying in %d reconcile tick(s).";

    if (backoff.m_consecutive_failures == 1 || interval_ticks != previous_interval_ticks) {
        normal_log(message,
                   __func__,
                   endpoint.ToString().c_str(),
                   backoff.m_consecutive_failures,
                   interval_ticks);
    } else {
        debug_log(message,
                  __func__,
                  endpoint.ToString().c_str(),
                  backoff.m_consecutive_failures,
                  interval_ticks);
    }
}

ShellKind IdleSourcePool::DebounceShellObservation(ShellKind observed)
{
    // Any sighting of a shell ends the run, including a sighting of a different shell: a KDE-to-GNOME
    // transition is a shell that is present, not a shell that is going away.
    if (observed != ShellKind::NONE) {
        m_shell_absence_observations = 0;

        return observed;
    }

    // Absence confirming absence. There is nothing to protect, and counting here would mean a shell
    // that appeared after a long tty session had to be un-counted before anything could act on it.
    if (m_shell_kind == ShellKind::NONE) {
        m_shell_absence_observations = 0;

        return ShellKind::NONE;
    }

    ++m_shell_absence_observations;

    if (m_shell_absence_observations < SHELL_ABSENCE_OBSERVATIONS_REQUIRED) {
        debug_log("INFO: %s: No desktop shell was detected on this reconcile (%d of %d consecutive). "
                  "Keeping the shell that is being tracked until the absence is confirmed.",
                  __func__,
                  m_shell_absence_observations,
                  SHELL_ABSENCE_OBSERVATIONS_REQUIRED);

        return m_shell_kind;
    }

    // Confirmed. Reset here rather than on the next observation, so that a shell which comes back and
    // goes away again gets a full count of its own.
    m_shell_absence_observations = 0;

    return ShellKind::NONE;
}

void IdleSourcePool::UpdateShellSourceContext()
{
    if (!m_shell_source) {
        return;
    }

    // The cast fails for a shell source that does not care, which is a legitimate implementation and
    // not an error.
    ShellSourceContext* context = dynamic_cast<ShellSourceContext*>(m_shell_source.get());

    if (context == nullptr) {
        return;
    }

    // CANDIDATES, NOT LIVE SOURCES, AND THE DIFFERENCE IS THE WHOLE POINT OF THIS FLAG.
    //
    // What the shell is asking is "am I a Wayland shell", because that is what decides whether a KDE
    // shell still has a GetSessionIdleTime to call. The honest answer is "a Wayland compositor is
    // running", and a socket in $XDG_RUNTIME_DIR is exactly that evidence. Whether WE can read an idle
    // time out of that compositor is a different and much narrower question, and it is false in
    // several ordinary situations that have nothing to do with the session's protocol: the compositor
    // may not advertise ext_idle_notifier_v1 at all, the candidate may be part way up a validation
    // backoff ladder, or its source may have died and not yet been rebuilt.
    //
    // Answering the first question with the second is what this fixes, and it was measured. On a
    // Plasma 6 Wayland desktop where no Wayland source was live, the shell concluded it was KDE on X11
    // and called GetIdleTimeKdeDBus() on every tick. Plasma 6 removed that method on Wayland, so every
    // call failed and logged: 20 error lines in a 20 second run, unbounded for the life of the process,
    // for a session whose protocol had never been in doubt.
    //
    // Misclassifying in the other direction stays benign, which is what makes candidates the right
    // input rather than merely the safer one. A KDE X11 session that happens to share the machine with
    // an unrelated Wayland compositor -- a nested weston, a headless remote desktop -- suppresses a
    // shell value that ksmserver derives from XScreenSaver anyway, and which the X11 endpoint source is
    // therefore already reporting. Nothing is lost, and inhibition does not run through here at all.
    context->SetWaylandEndpointPresent(m_wayland_candidate_present);
}

} // namespace IdleDetect
