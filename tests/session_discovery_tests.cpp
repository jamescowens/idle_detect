/*
 * Copyright (C) 2025-2026 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <gtest/gtest.h>
#include <session_discovery.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace IdleDetect;

namespace {

//!
//! \brief Creates a real unix domain socket at the given path so discovery's socket check
//! sees the same file type it will see in production.
//!
void MakeSocket(const fs::path& path)
{
    const std::string path_string = path.string();

    sockaddr_un addr{};
    ASSERT_LT(path_string.size(), sizeof(addr.sun_path));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_NE(fd, -1);

    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_string.c_str(), sizeof(addr.sun_path) - 1);

    const int bind_result = bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    // Close before asserting so a bind failure does not also leak the descriptor.
    close(fd);

    ASSERT_EQ(bind_result, 0);
}

//!
//! \brief RAII temporary directory.
//!
class TempDir
{
public:
    TempDir()
    {
        m_path = fs::temp_directory_path() / fs::path("idle_detect_test_" + std::to_string(getpid())
                                                      + "_" + std::to_string(++s_counter));
        fs::create_directories(m_path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }

    const fs::path& Path() const { return m_path; }

private:
    fs::path m_path;
    static int s_counter;
};

int TempDir::s_counter = 0;

//!
//! \brief RAII working directory change, so a test can prove that a relative path was NOT resolved
//! against the working directory.
//!
class ScopedWorkingDirectory
{
public:
    explicit ScopedWorkingDirectory(const fs::path& path)
        : m_previous(fs::current_path())
    {
        fs::current_path(path);
    }

    ~ScopedWorkingDirectory()
    {
        std::error_code ec;
        fs::current_path(m_previous, ec);
    }

    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

private:
    fs::path m_previous;
};

} // namespace

//
// X11 display normalization
//

TEST(NormalizeX11Display, AcceptsCanonicalForm)
{
    EXPECT_EQ(NormalizeX11Display(":1").value(), ":1");
}

TEST(NormalizeX11Display, StripsScreenSuffix)
{
    EXPECT_EQ(NormalizeX11Display(":1.0").value(), ":1");
}

TEST(NormalizeX11Display, AcceptsSocketBasename)
{
    EXPECT_EQ(NormalizeX11Display("X2").value(), ":2");
}

TEST(NormalizeX11Display, RejectsGarbage)
{
    EXPECT_FALSE(NormalizeX11Display("").has_value());
    EXPECT_FALSE(NormalizeX11Display("nonsense").has_value());
    EXPECT_FALSE(NormalizeX11Display(":").has_value());
    EXPECT_FALSE(NormalizeX11Display(":abc").has_value());
}

TEST(NormalizeX11Display, RejectsRemoteDisplays)
{
    // host:0 is a TCP display; XScreenSaver against it is not this daemon's business.
    EXPECT_FALSE(NormalizeX11Display("somehost:0").has_value());
}

TEST(NormalizeX11Display, StripsLeadingZeros)
{
    // Without this, ":007" and ":7" are two distinct Endpoints for one display, and both get
    // probed.
    EXPECT_EQ(NormalizeX11Display(":007").value(), ":7");
    EXPECT_EQ(NormalizeX11Display("X007").value(), ":7");
    EXPECT_EQ(NormalizeX11Display(":010").value(), ":10");
}

TEST(NormalizeX11Display, KeepsZeroDisplay)
{
    // Stripping zeros must not strip the display number away entirely.
    EXPECT_EQ(NormalizeX11Display(":0").value(), ":0");
    EXPECT_EQ(NormalizeX11Display(":00").value(), ":0");
    EXPECT_EQ(NormalizeX11Display("X0").value(), ":0");
}

//
// Wayland socket discovery
//

TEST(DiscoverEndpoints, FindsWaylandSocket)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::WAYLAND);
    EXPECT_EQ(endpoints.begin()->m_identifier, "wayland-0");
}

TEST(DiscoverEndpoints, IgnoresWaylandLockFiles)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");

    // A real socket, not a regular file. In production wayland-0.lock is a regular file and
    // the socket check alone would reject it, which would leave the ".lock" suffix guard
    // completely untested. Making it a socket here means the suffix guard is the only thing
    // that can reject this entry.
    MakeSocket(runtime.Path() / "wayland-0.lock");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, "wayland-0");
}

TEST(DiscoverEndpoints, FindsMultipleWaylandSockets)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");
    MakeSocket(runtime.Path() / "wayland-1");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    EXPECT_EQ(DiscoverEndpoints(hints).size(), 2u);
}

TEST(DiscoverEndpoints, IgnoresNonSocketFilesNamedLikeWayland)
{
    TempDir runtime;
    std::ofstream(runtime.Path() / "wayland-9").put('\n');

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, IgnoresRuntimeDirSocketsThatAreNotWaylandSockets)
{
    TempDir runtime;

    // A real $XDG_RUNTIME_DIR is dense with unix sockets that have nothing to do with a
    // compositor. Without the "wayland-" prefix guard every one of these is offered up as a
    // Wayland endpoint and then probed.
    MakeSocket(runtime.Path() / "bus");
    MakeSocket(runtime.Path() / "pipewire-0");
    MakeSocket(runtime.Path() / "gnupg");
    MakeSocket(runtime.Path() / "wayland-0");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::WAYLAND);
    EXPECT_EQ(endpoints.begin()->m_identifier, "wayland-0");
}

//
// Wayland hints. The directory scan matches the "wayland-" prefix, which is a convention rather
// than a rule, so a compositor that named its socket anything else is reachable only through a
// WAYLAND_DISPLAY hint. Losing these is a coverage regression, not a cosmetic one.
//

TEST(DiscoverEndpoints, FindsWaylandSocketNamedByRelativeHint)
{
    TempDir runtime;

    // "weston --socket=mysession". The prefix guard in the directory scan rejects this name, so the
    // hint is the only thing that can find it.
    MakeSocket(runtime.Path() / "mysession");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {"mysession"};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::WAYLAND);
    EXPECT_EQ(endpoints.begin()->m_identifier, "mysession");
}

TEST(DiscoverEndpoints, FindsWaylandSocketNamedByAbsoluteHintOutsideRuntimeDir)
{
    TempDir runtime;
    TempDir elsewhere;
    MakeSocket(elsewhere.Path() / "compositor-socket");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    // The Wayland specification allows WAYLAND_DISPLAY to be an absolute path, and libwayland uses it
    // as given rather than resolving it against $XDG_RUNTIME_DIR. A socket in another directory
    // entirely is therefore legal and is invisible to every scan discovery performs.
    hints.m_wayland_display_hints = {(elsewhere.Path() / "compositor-socket").string()};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::WAYLAND);

    // Identified by the full path, because that is the string wl_display_connect() has to be handed:
    // the basename alone would send it looking in $XDG_RUNTIME_DIR, where this socket is not.
    EXPECT_EQ(endpoints.begin()->m_identifier,
              fs::canonical(elsewhere.Path() / "compositor-socket").string());
}

TEST(DiscoverEndpoints, DeduplicatesAbsoluteWaylandHintAgainstTheDirectoryScan)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();

    // The same socket the scan finds, spelled as an absolute path. Two spellings of one path must not
    // become two endpoints: that would start two Wayland monitors against one compositor, each with
    // its own connection and thread, and report the same screen twice into the aggregate.
    hints.m_wayland_display_hints = {(runtime.Path() / "wayland-0").string()};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, "wayland-0");
}

TEST(DiscoverEndpoints, DeduplicatesRelativeWaylandHintAgainstTheDirectoryScan)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {"wayland-0"};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, "wayland-0");
}

TEST(DiscoverEndpoints, ResolvesWaylandHintWithASubdirectory)
{
    TempDir runtime;
    fs::create_directories(runtime.Path() / "nested");
    MakeSocket(runtime.Path() / "nested" / "wayland-1");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {"nested/wayland-1"};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);

    // Identified by its resolved path rather than collapsed to the basename. Collapsing would name a
    // socket directly in $XDG_RUNTIME_DIR, which is not where this one is, so the endpoint would be
    // unreachable again by a different route.
    EXPECT_EQ(endpoints.begin()->m_identifier,
              fs::canonical(runtime.Path() / "nested" / "wayland-1").string());
}

TEST(DiscoverEndpoints, FollowsASymlinkedWaylandHintToASocketTheScanCannotSee)
{
    TempDir runtime;
    TempDir elsewhere;
    MakeSocket(elsewhere.Path() / "compositor-socket");
    fs::create_symlink(elsewhere.Path() / "compositor-socket", runtime.Path() / "mysession");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {"mysession"};

    auto endpoints = DiscoverEndpoints(hints);

    // Nothing but the hint can reach this socket: it is outside the runtime directory, so the scan does
    // not see it, and the symlink standing in for it inside the runtime directory is not a socket, so an
    // lstat() of the hinted path rejects it too. The connect this candidate is validated by follows
    // symlinks, so resolving the hint the same way is what keeps the two consistent.
    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::WAYLAND);
    EXPECT_EQ(endpoints.begin()->m_identifier,
              fs::canonical(elsewhere.Path() / "compositor-socket").string());
}

TEST(DiscoverEndpoints, SymlinkedWaylandHintCollapsesOntoTheSocketItPointsAt)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");
    fs::create_symlink("wayland-0", runtime.Path() / "mysession");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {"mysession"};

    auto endpoints = DiscoverEndpoints(hints);

    // The hint and the directory scan name one compositor by two routes. Treating them as two endpoints
    // would open two connections and run two monitor threads against a single compositor, and report the
    // same screen into the aggregate twice. The symlink itself is not a socket -- the scan's lstat()
    // rejects it -- so following it is also the only way the hint resolves at all.
    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, "wayland-0");
}

TEST(DiscoverEndpoints, IgnoresWaylandHintThatNamesNoSocket)
{
    TempDir runtime;

    // A regular file at the hinted path, so the socket check is the only guard that can reject it.
    std::ofstream(runtime.Path() / "not-a-socket").put('\n');

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {"not-a-socket", "never-existed", "/absolutely/not/there", ""};

    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, IgnoresRelativeWaylandHintWithNoRuntimeDir)
{
    TempDir working;
    MakeSocket(working.Path() / "mysession");

    // The socket exists in the process's working directory and nowhere else. That is what gives this
    // test teeth: dropping the guard makes "" / "mysession" resolve to a working-directory-relative
    // path, which lstat() then finds, so the endpoint appears. Without the socket being reachable that
    // way the test would pass whatever the code did.
    ScopedWorkingDirectory working_directory(working.Path());

    DiscoveryHints hints;
    // m_xdg_runtime_dir deliberately left empty, as it is when XDG_RUNTIME_DIR is unset.
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {"mysession"};

    // A relative name has no meaning without the directory it is relative to, and discovery must not
    // depend on where the daemon happened to be started from.
    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, AbsoluteWaylandHintSurvivesAnUnsetRuntimeDir)
{
    TempDir elsewhere;
    MakeSocket(elsewhere.Path() / "wayland-9");

    DiscoveryHints hints;
    // m_xdg_runtime_dir deliberately left empty.
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {(elsewhere.Path() / "wayland-9").string()};

    // An absolute path needs no runtime directory to resolve against, so the missing one that
    // disqualifies a relative hint must not disqualify this one.
    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, fs::canonical(elsewhere.Path() / "wayland-9").string());
}

TEST(DiscoverEndpoints, UnionsWaylandHintsWithTheDirectoryScan)
{
    TempDir runtime;
    MakeSocket(runtime.Path() / "wayland-0");
    MakeSocket(runtime.Path() / "mysession");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_uid = getuid();
    hints.m_wayland_display_hints = {"mysession"};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 2u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::WAYLAND, "wayland-0"})), 1u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::WAYLAND, "mysession"})), 1u);
}

TEST(DiscoverEndpoints, MissingRuntimeDirIsNotAnError)
{
    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = "/nonexistent/path/for/test";
    hints.m_uid = getuid();

    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

//
// X11 discovery: the hints are unreliable in complementary ways, so they are unioned.
//

TEST(DiscoverEndpoints, FindsX11SocketOwnedByUs)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::X11);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":1");
}

TEST(DiscoverEndpoints, IgnoresNonSocketFilesNamedLikeAnXSocket)
{
    TempDir x11;
    std::ofstream(x11.Path() / "X1").put('\n');

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();

    // The regular file is named like an X socket and is owned by us, so the socket check is
    // the only guard that can reject it.
    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, IgnoresX11DirSocketsNotNamedLikeAnXSocket)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    // Y1 and notX are what a reader expects this guard to be about, but they are rejected
    // twice over: the name check here and NormalizeX11Display(), which accepts only ':' and
    // 'X' leading characters. ":9" is the name that isolates the guard, because it survives
    // normalization and would be admitted as display :9 if the guard were removed.
    MakeSocket(x11.Path() / "Y1");
    MakeSocket(x11.Path() / "notX");
    MakeSocket(x11.Path() / ":9");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::X11);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":1");
}

TEST(DiscoverEndpoints, RejectsX11SocketOwnedByAnotherUser)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X0");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    // Pretend we are a different user than the socket's owner. /tmp/.X11-unix is world
    // visible and holds the root-owned greeter socket alongside ours.
    hints.m_uid = getuid() + 1;

    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoveryHints, DefaultUidIsExplicitlyInvalidRatherThanRoot)
{
    // 0 is a legal uid, so it cannot double as "unset". A default of 0 does not disable the
    // ownership filter for a caller that forgets this field, it inverts it: our own sockets
    // are rejected and the display manager's root-owned greeter socket is accepted.
    DiscoveryHints hints;

    EXPECT_EQ(hints.m_uid, static_cast<uid_t>(-1));
    EXPECT_NE(hints.m_uid, static_cast<uid_t>(0));
}

TEST(DiscoverEndpoints, UnsetUidHintYieldsNoX11Endpoints)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    // m_uid deliberately left at its default.

    // The socket is a valid X socket owned by this very process, so it is exactly what the
    // scan would return if the scan ran. An unset uid must mean "skip the scan", never
    // "guess the caller's uid and scan anyway", which is the tempting-but-wrong fix: it
    // silently trusts whatever this process happens to run as.
    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, UnsetUidHintStillHonorsDisplayHints)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_env_displays = {":1"};
    // m_uid deliberately left at its default.

    // Skipping the socket scan must not disable the hints that carry no ownership question. The socket
    // exists, so the hint's own existence requirement is satisfied; the uid is the only thing missing, and
    // it is not the hint path's business.
    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":1");
}

TEST(DiscoverEndpoints, UsesEnvDisplayHintWhenSocketIsNotOurs)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid() + 1;      // socket filter rejects it
    hints.m_env_displays = {":1"};   // but the systemd manager environment knows about it

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":1");
}

//
// X11 hints must name a socket that is really there. A DISPLAY value is exported when a graphical
// session starts and never unexported when it ends, so on its own it is evidence that a display once
// existed, not that one exists now.
//

TEST(DiscoverEndpoints, AdmitsEnvDisplayHintWhoseSocketExists)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X7");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();
    hints.m_env_displays = {":7"};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::X11);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":7");
}

TEST(DiscoverEndpoints, RejectsEnvDisplayHintWithNoSocket)
{
    TempDir x11;

    // Nothing at all in the socket directory: no X server has ever run for this display. The hint is the
    // stale export left behind by the last session.
    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();
    hints.m_env_displays = {":7"};

    // Admitting it costs a validation attempt on the backoff ladder for the life of the daemon, against a
    // display that provably cannot answer.
    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, RejectsLogindDisplayHintWithNoSocket)
{
    TempDir x11;

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();
    hints.m_logind_displays = {":7"};

    // Same requirement on the other hint source. logind keeps a Display property on a session object that
    // outlives the X server just as readily.
    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, RejectsAStaleDisplayHintWhileKeepingTheLiveOne)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();

    // A machine that has been logged in and out of a few times accumulates these. Only :1 is real.
    hints.m_env_displays = {":1", ":3"};
    hints.m_logind_displays = {":5"};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":1");
}

TEST(DiscoverEndpoints, AdmitsHintForASocketTheUidFilterRejects)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X0");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();

    // The socket belongs to getuid(); pointing the filter at a different uid makes the scan see it exactly
    // as it sees the ROOT-OWNED socket a display manager started X server leaves in /tmp/.X11-unix. The
    // scan is right to reject that -- the directory is world visible and it cannot tell that socket from
    // another user's -- and this hint is the only thing left that can recover the display.
    hints.m_uid = getuid() + 1;
    hints.m_env_displays = {":0"};

    auto endpoints = DiscoverEndpoints(hints);

    // If the socket existence check ever grows an ownership test, this is the test that fails, and the
    // display manager case -- which is the whole reason the environment hint is unioned with the scan --
    // silently stops working. Existence and S_ISSOCK yes; ownership no.
    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_kind, EndpointKind::X11);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":0");
}

TEST(DiscoverEndpoints, RejectsHintNamingSomethingThatIsNotASocket)
{
    TempDir x11;

    // Named exactly like an X socket and owned by us, so the file type check is the only guard that can
    // reject it.
    std::ofstream(x11.Path() / "X7").put('\n');

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();
    hints.m_env_displays = {":7"};

    EXPECT_TRUE(DiscoverEndpoints(hints).empty());
}

TEST(DiscoverEndpoints, AdmitsDisplayHintsUncheckedWithNoSocketDir)
{
    DiscoveryHints hints;
    // m_x11_socket_dir deliberately left empty.
    hints.m_uid = getuid();
    hints.m_env_displays = {":3"};
    hints.m_logind_displays = {":4"};

    // An input that cannot be evaluated must not be evaluated approximately, which is the same rule that
    // skips the socket scan without a uid. With no directory to look in there is nothing to check against,
    // and rejecting on that basis would discard every hint rather than only the stale ones.
    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 2u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::X11, ":3"})), 1u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::X11, ":4"})), 1u);
}

TEST(DiscoverEndpoints, UnionsHintsAndDeduplicates)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");
    MakeSocket(x11.Path() / "X2");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();
    hints.m_env_displays = {":1"};       // duplicate of a scanned socket
    hints.m_logind_displays = {":2"};    // duplicate of a scanned socket, by a different route

    // Three routes to two displays. A hint that names a display the scan did not produce is covered by
    // UsesEnvDisplayHintWhenSocketIsNotOurs; both hints here name sockets that really exist, because a
    // hint whose socket does not exist is no longer a candidate at all.
    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 2u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::X11, ":1"})), 1u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::X11, ":2"})), 1u);
}

TEST(DiscoverEndpoints, IgnoresUnparseableHints)
{
    DiscoveryHints hints;
    hints.m_uid = getuid();
    // No socket directory, so the hints are admitted unchecked and normalization is the only guard under
    // test here.
    hints.m_env_displays = {"", "garbage", ":3"};

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints.begin()->m_identifier, ":3");
}

TEST(DiscoverEndpoints, CombinesWaylandAndX11)
{
    TempDir runtime;
    TempDir x11;
    MakeSocket(runtime.Path() / "wayland-0");
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_xdg_runtime_dir = runtime.Path();
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();

    EXPECT_EQ(DiscoverEndpoints(hints).size(), 2u);
}
