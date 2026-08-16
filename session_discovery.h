/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef SESSION_DISCOVERY_H
#define SESSION_DISCOVERY_H

#include <idle_source.h>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

namespace IdleDetect {

//!
//! \brief Normalizes an X display specifier to the canonical ":N" form.
//!
//! Accepts ":N", ":N.S", and the socket basename form "XN". Rejects empty input, remote
//! (host-qualified) displays, and anything without a numeric display number.
//!
//! \param raw display specifier from a discovery hint
//! \return canonical ":N" form, or std::nullopt if unparseable
//!
std::optional<std::string> NormalizeX11Display(const std::string& raw);

//!
//! \brief Inputs to endpoint discovery. Every field is an independently unreliable hint;
//! none is required to be populated.
//!
struct DiscoveryHints {
    //! \brief Directory searched for wayland-* sockets. Typically $XDG_RUNTIME_DIR.
    std::filesystem::path m_xdg_runtime_dir;

    //! \brief Directory searched for X sockets. Typically /tmp/.X11-unix.
    std::filesystem::path m_x11_socket_dir;

    //! \brief DISPLAY values read from the systemd user manager Environment property.
    std::vector<std::string> m_env_displays;

    //! \brief Display values from logind graphical sessions. Optional enrichment only.
    std::vector<std::string> m_logind_displays;

    //! \brief Our uid. X socket candidates not owned by this uid are rejected, because
    //! /tmp/.X11-unix is world visible and contains other users' sockets.
    uid_t m_uid = 0;
};

//!
//! \brief Builds the candidate endpoint set from the union of all hints.
//!
//! Discovery is deliberately over-inclusive; callers validate each candidate by connecting
//! to it. No single hint is load-bearing.
//!
//! \param hints discovery inputs
//! \return deduplicated candidate endpoints
//!
std::set<Endpoint> DiscoverEndpoints(const DiscoveryHints& hints);

} // namespace IdleDetect

#endif // SESSION_DISCOVERY_H
