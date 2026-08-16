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
