#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Container/renderer/core/RendererMcpServer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <stdexcept>

namespace {

using container::renderer::RendererMcpServer;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::string_view token = "test-loopback-renderer-mcp-transport-token";

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket invalidSocket = -1;
#endif

class Client {
 public:
  explicit Client(uint16_t port) : port_(port) {
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == invalidSocket) throw std::runtime_error("Could not create the MCP test socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      closeSocket();
      throw std::runtime_error("Could not connect to the MCP test server");
    }
  }

  ~Client() { closeSocket(); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void request(const Json& message) const {
    const auto body = message.dump();
    const std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port_) +
        "\r\nAuthorization: Bearer " + std::string(token) +
        "\r\nAccept: application/json, text/event-stream\r\nContent-Type: application/json\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < request.size()) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto count = send(socket_, request.data() + sent, static_cast<int>(request.size() - sent), flags);
      if (count <= 0) throw std::runtime_error("Could not send the MCP test request");
      sent += static_cast<size_t>(count);
    }
  }

  void disconnect(bool reset) {
    if (reset) {
      const linger abortive{1, 0};
      if (setsockopt(socket_, SOL_SOCKET, SO_LINGER,
                     reinterpret_cast<const char*>(&abortive), sizeof(abortive)) != 0)
        throw std::runtime_error("Could not configure an abortive MCP test close");
    }
    closeSocket();
  }

  bool readableWithin(std::chrono::milliseconds wait) const {
#ifndef _WIN32
    if (socket_ >= FD_SETSIZE) throw std::runtime_error("MCP test socket exceeds FD_SETSIZE");
#endif
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket_, &readable);
    timeval timeout{static_cast<long>(wait.count() / 1000), static_cast<long>((wait.count() % 1000) * 1000)};
#ifdef _WIN32
    const int descriptors = 0;
#else
    const int descriptors = socket_ + 1;
#endif
    const int result = select(descriptors, &readable, nullptr, nullptr, &timeout);
    if (result < 0) throw std::runtime_error("Could not poll the MCP test socket");
    return result > 0;
  }

  Json response() const {
    std::string response;
    const auto deadline = Clock::now() + 4s;
    for (;;) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
      if (remaining <= 0ms || !readableWithin(remaining))
        throw std::runtime_error("MCP test client timed out waiting for a response");
      std::array<char, 1024> bytes{};
      const auto count = recv(socket_, bytes.data(), static_cast<int>(bytes.size()), 0);
      if (count < 0) throw std::runtime_error("MCP test client could not read the response");
      if (count == 0) break;
      response.append(bytes.data(), static_cast<size_t>(count));
    }
    const auto headers = response.find("\r\n\r\n");
    if (!response.starts_with("HTTP/1.1 200 ") || headers == std::string::npos)
      throw std::runtime_error("Unexpected MCP test response: " + response);
    return Json::parse(response.substr(headers + 4));
  }

 private:
  void closeSocket() noexcept {
    if (socket_ == invalidSocket) return;
#ifdef _WIN32
    closesocket(socket_);
#else
    close(socket_);
#endif
    socket_ = invalidSocket;
  }

  uint16_t port_;
  NativeSocket socket_{invalidSocket};
};

Json lightingRequest() {
  return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
          {"params", {{"name", "renderer_set_lighting"}, {"arguments", {{"environmentIntensity", 0.5}}}}}};
}

Json pingRequest() {
  return {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}};
}

std::string commandError(const Json& response) {
  EXPECT_EQ(response.at("id"), 1);
  const auto& result = response.at("result");
  EXPECT_EQ(result.at("isError"), true);
  return Json::parse(result.at("content").at(0).at("text").get<std::string>()).at("message").get<std::string>();
}

class RendererMcpTransport : public ::testing::Test {
 protected:
  bool pumpUntilReadable(const Client& client, const RendererMcpServer::OwnerHandler& handler,
                         std::chrono::milliseconds timeout = 1s) {
    const auto deadline = Clock::now() + timeout;
    do {
      server_.pump(handler);
      if (client.readableWithin(10ms)) return true;
    } while (Clock::now() < deadline);
    return false;
  }

  void testDisconnectBeforeAcceptance(bool reset) {
    Client abandoned(server_.port());
    abandoned.request(lightingRequest());
    const Client follower(server_.port());
    follower.request(pingRequest());
    ASSERT_FALSE(follower.readableWithin(100ms)) << "The first command must hold the listener while the owner is idle";
    abandoned.disconnect(reset);
    int calls = 0;
    const bool recovered = pumpUntilReadable(follower, [&](std::string_view, const Json&) {
      ++calls;
      return Json::object();
    });
    EXPECT_EQ(calls, 0);
    ASSERT_TRUE(recovered) << "A closed peer must release the listener before the command deadline";
    EXPECT_EQ(follower.response().at("id"), 2);
  }

  RendererMcpServer server_{0, std::string(token)};
};

TEST_F(RendererMcpTransport, GracefulDisconnectBeforeAcceptanceNeverReachesOwner) {
  testDisconnectBeforeAcceptance(false);
}

TEST_F(RendererMcpTransport, ResetBeforeAcceptanceNeverReachesOwner) {
  testDisconnectBeforeAcceptance(true);
}

TEST_F(RendererMcpTransport, TimeoutBeforeAcceptanceNeverReachesOwner) {
  const Client expired(server_.port());
  expired.request(lightingRequest());
  EXPECT_EQ(commandError(expired.response()), "Renderer did not accept the command before the request timed out");

  const Client following(server_.port());
  following.request(lightingRequest());
  int calls = 0;
  ASSERT_TRUE(pumpUntilReadable(following, [&](std::string_view name, const Json& arguments) {
    EXPECT_EQ(name, "renderer_set_lighting");
    EXPECT_EQ(arguments.at("environmentIntensity"), 0.5);
    ++calls;
    return Json::object();
  }));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(following.response().at("result").at("isError"), false);
}

TEST_F(RendererMcpTransport, AcceptedCommandCompletesAfterDisconnect) {
  Client command(server_.port());
  command.request(lightingRequest());
  const Client follower(server_.port());
  follower.request(pingRequest());
  int completed = 0;
  ASSERT_TRUE(pumpUntilReadable(follower, [&](std::string_view name, const Json&) {
    EXPECT_EQ(name, "renderer_set_lighting");
    command.disconnect(true);
    ++completed;
    return Json::object();
  }));
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(follower.response().at("id"), 2);
}

TEST_F(RendererMcpTransport, AcceptedCommandResponseTimeoutReportsAcceptance) {
  const Client command(server_.port());
  command.request(lightingRequest());
  const Client follower(server_.port());
  follower.request(pingRequest());
  int completed = 0;
  ASSERT_TRUE(pumpUntilReadable(follower, [&](std::string_view name, const Json&) {
    EXPECT_EQ(name, "renderer_set_lighting");
    EXPECT_EQ(commandError(command.response()), "Renderer accepted the command, but its response timed out");
    ++completed;
    return Json::object();
  }, 4s));
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(follower.response().at("id"), 2);
}

} // namespace
