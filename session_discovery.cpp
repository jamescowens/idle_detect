/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <session_discovery.h>

#include <cctype>
#include <cstddef>
#include <system_error>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace IdleDetect {

namespace {

//!
//! \brief Returns true if every character is an ASCII digit and the string is non-empty.
//!
bool IsAllDigits(const std::string& value)
{
    if (value.empty()) {
        return false;
    }

    for (const unsigned char c : value) {
        if (!std::isdigit(c)) {
            return false;
        }
    }

    return true;
}

//!
//! \brief Lists the entries of a directory without throwing.
//!
//! Discovery runs against directories that can disappear or become unreadable underneath it,
//! so every filesystem error - including one raised part way through iteration - is treated
//! as "nothing further to see here" rather than as a failure.
//!
//! \param dir directory to list
//! \return paths of the entries read before any error, empty if the directory is unusable
//!
std::vector<fs::path> ListDirectory(const fs::path& dir)
{
    std::vector<fs::path> paths;

    std::error_code ec;
    fs::directory_iterator iter(dir, ec);
    const fs::directory_iterator end;

    while (!ec && iter != end) {
        paths.push_back(iter->path());
        iter.increment(ec);
    }

    return paths;
}

//!
//! \brief Returns true if the path exists and is a unix domain socket.
//!
bool IsSocket(const fs::path& path)
{
    struct stat st{};

    if (lstat(path.c_str(), &st) != 0) {
        return false;
    }

    return S_ISSOCK(st.st_mode);
}

//!
//! \brief Returns true if the path exists and is owned by the given uid.
//!
bool IsOwnedBy(const fs::path& path, uid_t uid)
{
    struct stat st{};

    if (lstat(path.c_str(), &st) != 0) {
        return false;
    }

    return st.st_uid == uid;
}

//!
//! \brief Turns a WAYLAND_DISPLAY hint into an endpoint, if it names a socket that exists.
//!
//! Both forms the Wayland specification allows are handled, because both occur: an absolute path is
//! taken as written, and a relative name is resolved against the runtime directory. That is precisely
//! what libwayland's wl_display_connect() does with the same string, so a hint that resolves to a socket
//! here names the same socket there.
//!
//! Whatever the form, the path is then resolved to its real location before anything is decided about
//! it. That is what the eventual connect() does with the same string, and it is what lets two spellings
//! of one socket -- "wayland-0", "/run/user/1000/wayland-0", a symlink aliasing either -- collapse into
//! one endpoint instead of starting one compositor connection and monitor thread each.
//!
//! The identifier is then the socket's basename when the socket sits directly in the runtime directory,
//! which is what deduplicates it against the directory scan, and the resolved absolute path otherwise.
//! Both are strings wl_display_connect() accepts, so the endpoint stays reachable either way, and both
//! are stable across ticks, which matters because the identifier is what keys the pool's source and
//! backoff maps.
//!
//! A relative hint with no runtime directory to resolve against is dropped rather than guessed at, for
//! the same reason DiscoverEndpoints() skips the X socket scan without a uid: an input that cannot be
//! evaluated must not be evaluated approximately. Resolving it against the process's working directory
//! instead would make discovery depend on where the daemon was started from.
//!
//! \param runtime_dir $XDG_RUNTIME_DIR, or empty if it is unknown
//! \param hint raw WAYLAND_DISPLAY value
//! \return the endpoint, or std::nullopt if the hint does not resolve to an existing socket
//!
std::optional<Endpoint> ResolveWaylandHint(const fs::path& runtime_dir, const std::string& hint)
{
    if (hint.empty()) {
        return std::nullopt;
    }

    const fs::path hint_path(hint);
    fs::path socket_path;

    if (hint_path.is_absolute()) {
        socket_path = hint_path;
    } else {
        if (runtime_dir.empty()) {
            return std::nullopt;
        }

        socket_path = runtime_dir / hint_path;
    }

    // A path that cannot be resolved does not exist, which is the same outcome as not being a socket, so
    // there is nothing to distinguish here.
    std::error_code path_error;
    const fs::path resolved = fs::canonical(socket_path, path_error);

    if (path_error) {
        return std::nullopt;
    }

    // The same check the directory scan applies. A hint naming something that is not a socket -- a stale
    // path, a lock file, a directory -- is not an endpoint, and this is the only filter there is, since a
    // hint carries no name convention to test against.
    if (!IsSocket(resolved)) {
        return std::nullopt;
    }

    if (!runtime_dir.empty()) {
        std::error_code dir_error;
        const fs::path resolved_runtime_dir = fs::canonical(runtime_dir, dir_error);

        if (!dir_error && resolved.parent_path() == resolved_runtime_dir) {
            return Endpoint{EndpointKind::WAYLAND, resolved.filename().string()};
        }
    }

    return Endpoint{EndpointKind::WAYLAND, resolved.string()};
}

//!
//! \brief Adds an X display hint to the set if it normalizes successfully.
//!
void InsertX11Candidate(std::set<Endpoint>& endpoints, const std::string& raw)
{
    std::optional<std::string> normalized = NormalizeX11Display(raw);

    if (normalized.has_value()) {
        endpoints.insert(Endpoint{EndpointKind::X11, *normalized});
    }
}

} // anonymous namespace

std::optional<std::string> NormalizeX11Display(const std::string& raw)
{
    if (raw.empty()) {
        return std::nullopt;
    }

    std::string number;

    if (raw[0] == ':') {
        // ":N" or ":N.S" -- take everything up to an optional screen suffix.
        const size_t dot = raw.find('.', 1);
        number = raw.substr(1, (dot == std::string::npos) ? std::string::npos : dot - 1);
    } else if (raw[0] == 'X') {
        // Socket basename form, "XN".
        number = raw.substr(1);
    } else {
        // Host-qualified or otherwise not a local display.
        return std::nullopt;
    }

    if (!IsAllDigits(number)) {
        return std::nullopt;
    }

    // Canonicalize leading zeros, so ":007" and ":7" do not become two distinct Endpoints for
    // one display. An all-zero number collapses to a single "0", making ":0" and ":00" the
    // same canonical display.
    const size_t first_significant = number.find_first_not_of('0');

    if (first_significant == std::string::npos) {
        number = "0";
    } else {
        number.erase(0, first_significant);
    }

    return ":" + number;
}

std::set<Endpoint> DiscoverEndpoints(const DiscoveryHints& hints)
{
    std::set<Endpoint> endpoints;

    // --- Wayland: sockets named wayland-* in the runtime directory. ---
    if (!hints.m_xdg_runtime_dir.empty()) {
        for (const fs::path& path : ListDirectory(hints.m_xdg_runtime_dir)) {
            const std::string name = path.filename().string();

            if (name.rfind("wayland-", 0) != 0) {
                continue;
            }

            // wayland-0.lock sits beside wayland-0 and is a regular file, not a socket.
            if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".lock") == 0) {
                continue;
            }

            if (!IsSocket(path)) {
                continue;
            }

            endpoints.insert(Endpoint{EndpointKind::WAYLAND, name});
        }
    }

    // --- Wayland: names the environment declares, which the scan above cannot see. ---
    //
    // The scan matches on the "wayland-" prefix, which is a convention and not a rule, so a compositor
    // started as "weston --socket=mysession", a nested one, or one whose socket is outside the runtime
    // directory is reachable only through this hint. Unlike the X socket scan there is no ownership
    // filter here, and none is needed: these values are declarations from our own environment rather
    // than an enumeration of a world-visible directory, and connecting is what validates them.
    for (const std::string& hint : hints.m_wayland_display_hints) {
        std::optional<Endpoint> endpoint = ResolveWaylandHint(hints.m_xdg_runtime_dir, hint);

        if (endpoint.has_value()) {
            endpoints.insert(*endpoint);
        }
    }

    // --- X11: union of socket scan, systemd manager environment, and logind. ---
    //
    // The socket scan is skipped outright when the caller left m_uid at its invalid default,
    // because the ownership filter below is the only thing standing between us and another
    // user's socket, and a filter that cannot be evaluated must not be run. Guessing the
    // caller's uid here would be worse than discovering nothing: it would silently accept
    // whatever this process happens to run as. This translation unit has no logging
    // dependency by design, so the miss is silent. The environment and logind hints below
    // still contribute.
    if (!hints.m_x11_socket_dir.empty() && hints.m_uid != static_cast<uid_t>(-1)) {
        for (const fs::path& path : ListDirectory(hints.m_x11_socket_dir)) {
            const std::string name = path.filename().string();

            if (name.empty() || name[0] != 'X') {
                continue;
            }

            if (!IsSocket(path)) {
                continue;
            }

            // This directory is world visible and holds other users' sockets, including
            // the display manager's root-owned greeter socket.
            if (!IsOwnedBy(path, hints.m_uid)) {
                continue;
            }

            InsertX11Candidate(endpoints, name);
        }
    }

    for (const std::string& display : hints.m_env_displays) {
        InsertX11Candidate(endpoints, display);
    }

    for (const std::string& display : hints.m_logind_displays) {
        InsertX11Candidate(endpoints, display);
    }

    return endpoints;
}

} // namespace IdleDetect
