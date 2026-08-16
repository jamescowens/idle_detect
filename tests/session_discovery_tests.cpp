/*
 * Copyright (C) 2025 James C. Owens
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
    std::ofstream(runtime.Path() / "wayland-0.lock").put('\n');

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

TEST(DiscoverEndpoints, UnionsHintsAndDeduplicates)
{
    TempDir x11;
    MakeSocket(x11.Path() / "X1");

    DiscoveryHints hints;
    hints.m_x11_socket_dir = x11.Path();
    hints.m_uid = getuid();
    hints.m_env_displays = {":1"};       // duplicate of the socket
    hints.m_logind_displays = {":2"};    // additional, from optional enrichment

    auto endpoints = DiscoverEndpoints(hints);

    ASSERT_EQ(endpoints.size(), 2u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::X11, ":1"})), 1u);
    EXPECT_EQ(endpoints.count((Endpoint{EndpointKind::X11, ":2"})), 1u);
}

TEST(DiscoverEndpoints, IgnoresUnparseableHints)
{
    DiscoveryHints hints;
    hints.m_uid = getuid();
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
