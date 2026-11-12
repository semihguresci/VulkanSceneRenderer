#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Container/renderer/core/RendererMcpServer.h"
#include "Container/app/RendererLightingSettings.h"
#include "Container/renderer/core/RenderGraph.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/utility/Platform.h"
#include "Container/utility/RendererMcpLogs.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace container::renderer {
namespace {

using Json = nlohmann::json;
using Clock = RendererMcpMeasurement::Clock;
using namespace std::chrono_literals;
constexpr size_t maxHeaders = 8u * 1024u;
constexpr size_t maxBody = 64u * 1024u;
constexpr auto requestTimeout = 2s;
constexpr std::string_view currentProtocol = "2025-11-25";

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket invalidSocket = -1;
#endif

struct NetworkRuntime {
  NetworkRuntime() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      throw std::runtime_error("Could not initialize Winsock");
#endif
  }
  ~NetworkRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
};

struct Socket {
  NativeSocket handle{invalidSocket};
  explicit Socket(NativeSocket value) : handle(value) {}
  ~Socket() {
    if (handle == invalidSocket) return;
#ifdef _WIN32
    closesocket(handle);
#else
    close(handle);
#endif
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept : handle(std::exchange(other.handle, invalidSocket)) {}
};

bool makeNonblocking(NativeSocket socket) {
#ifdef _WIN32
  u_long enabled = 1;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool peerDisconnected(NativeSocket socket) {
  char byte{};
  const auto received = recv(socket, &byte, 1, MSG_PEEK);
  if (received >= 0) return received == 0;
#ifdef _WIN32
  const auto error = WSAGetLastError();
  return error != WSAEWOULDBLOCK && error != WSAEINTR;
#else
  return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
#endif
}

int socketReady(NativeSocket socket, bool writing, std::chrono::microseconds wait) {
#ifndef _WIN32
  if (socket >= FD_SETSIZE) return -1;
#endif
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(socket, &ready);
  timeval timeout{0, static_cast<long>(wait.count())};
#ifdef _WIN32
  const int descriptors = 0;
#else
  const int descriptors = socket + 1;
#endif
  return select(descriptors, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &timeout);
}

bool waitForSocket(NativeSocket socket, bool writing, Clock::time_point deadline,
                   std::stop_token stop) {
  while (!stop.stop_requested() && Clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
    const auto wait = std::clamp(remaining, 0us, 100000us);
    const int result = socketReady(socket, writing, wait);
    if (result != 0) return result > 0;
  }
  return false;
}

Socket makeListener(uint16_t requestedPort, uint16_t& boundPort) {
  Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (listener.handle == invalidSocket || !makeNonblocking(listener.handle))
    throw std::runtime_error("Could not create an MCP listening socket");
#ifdef _WIN32
  const BOOL exclusive = TRUE;
  if (setsockopt(listener.handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
    throw std::runtime_error("Could not reserve the MCP loopback port exclusively");
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(requestedPort);
  if (bind(listener.handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener.handle, 4) != 0)
    throw std::runtime_error("Could not listen for MCP on 127.0.0.1");
#ifdef _WIN32
  int size = sizeof(address);
#else
  socklen_t size = sizeof(address);
#endif
  if (getsockname(listener.handle, reinterpret_cast<sockaddr*>(&address), &size) != 0)
    throw std::runtime_error("Could not identify the MCP loopback port");
  boundPort = ntohs(address.sin_port);
  return listener;
}

std::string lowercase(std::string_view input) {
  std::string value(input);
  for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}

std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

struct HttpError : std::runtime_error {
  int code;
  HttpError(int status, std::string message) : std::runtime_error(std::move(message)), code(status) {}
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string, std::less<>> headers;
  std::string body;

  std::string_view header(std::string_view name) const {
    const auto found = headers.find(name);
    return found == headers.end() ? std::string_view{} : std::string_view(found->second);
  }
};

std::optional<HttpRequest> readRequest(NativeSocket socket, std::stop_token stop) {
  const auto deadline = Clock::now() + requestTimeout;
  std::string bytes;
  const auto receive = [&]() {
    if (!waitForSocket(socket, false, deadline, stop)) return false;
    std::array<char, 4096> buffer{};
    const auto received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0) return false;
    bytes.append(buffer.data(), static_cast<size_t>(received));
    return true;
  };
  size_t headerEnd;
  while ((headerEnd = bytes.find("\r\n\r\n")) == std::string::npos) {
    if (bytes.size() >= maxHeaders) throw HttpError(413, "MCP headers exceed their size limit");
    if (!receive()) return std::nullopt;
  }
  if (headerEnd > maxHeaders) throw HttpError(413, "MCP headers exceed their size limit");

  std::string_view headerBlock(bytes.data(), headerEnd);
  const auto lineEnd = headerBlock.find("\r\n");
  const auto line = headerBlock.substr(0, lineEnd);
  const auto methodEnd = line.find(' ');
  const auto pathEnd = methodEnd == std::string_view::npos ? methodEnd : line.find(' ', methodEnd + 1);
  if (pathEnd == std::string_view::npos || line.substr(pathEnd + 1) != "HTTP/1.1")
    throw HttpError(400, "Expected an HTTP/1.1 request");
  HttpRequest request{std::string(line.substr(0, methodEnd)),
                      std::string(line.substr(methodEnd + 1, pathEnd - methodEnd - 1))};
  if (lineEnd != std::string_view::npos) {
    headerBlock.remove_prefix(lineEnd + 2);
    while (!headerBlock.empty()) {
      const auto next = headerBlock.find("\r\n");
      const auto header = headerBlock.substr(0, next);
      const auto colon = header.find(':');
      if (!colon || colon == std::string_view::npos ||
          header.find_first_of("\r\n\0", 0, 3) != std::string_view::npos)
        throw HttpError(400, "Malformed MCP header");
      auto name = lowercase(header.substr(0, colon));
      if (!request.headers.emplace(std::move(name), trim(header.substr(colon + 1))).second)
        throw HttpError(400, "Duplicate MCP header");
      if (next == std::string_view::npos) break;
      headerBlock.remove_prefix(next + 2);
    }
  }
  if (request.headers.contains("transfer-encoding"))
    throw HttpError(400, "MCP requests require an explicit Content-Length");
  if (request.method != "POST") return request;

  const auto length = request.header("content-length");
  if (length.empty()) throw HttpError(400, "MCP POST requires Content-Length");
  size_t bodySize{};
  const auto parsed = std::from_chars(length.data(), length.data() + length.size(), bodySize);
  if (parsed.ec != std::errc{} || parsed.ptr != length.data() + length.size())
    throw HttpError(400, "MCP POST requires a valid Content-Length");
  if (bodySize > maxBody) throw HttpError(413, "MCP body exceeds its size limit");
  const auto bodyStart = headerEnd + 4;
  while (bytes.size() < bodyStart + bodySize)
    if (!receive()) return std::nullopt;
  if (bytes.size() != bodyStart + bodySize)
    throw HttpError(400, "Unexpected data after the MCP request body");
  request.body = bytes.substr(bodyStart);
  return request;
}

void sendResponse(NativeSocket socket, int status, std::string_view body, std::stop_token stop) {
  const auto label = status == 200 ? "OK" : status == 202 ? "Accepted" :
      status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" :
      status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" :
      status == 413 ? "Content Too Large" : "Bad Request";
  std::string message = "HTTP/1.1 " + std::to_string(status) + " " + label + "\r\n";
  message += "Connection: close\r\nCache-Control: no-store\r\n";
  if (status == 401) message += "WWW-Authenticate: Bearer\r\n";
  if (status == 405) message += "Allow: POST\r\n";
  if (!body.empty()) message += "Content-Type: application/json; charset=utf-8\r\n";
  message += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  message += body;

  const auto deadline = Clock::now() + requestTimeout;
  size_t sent = 0;
  while (sent < message.size() && waitForSocket(socket, true, deadline, stop)) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count = send(socket, message.data() + sent,
                            static_cast<int>(message.size() - sent), flags);
    if (count <= 0) return;
    sent += static_cast<size_t>(count);
  }
}

bool supportsProtocol(std::string_view version) {
  return version == currentProtocol || version == "2025-06-18" || version == "2025-03-26";
}

bool authorized(std::string_view supplied, std::string_view expected) {
  if (!supplied.starts_with("Bearer ")) return false;
  supplied.remove_prefix(7);
  if (supplied.size() != expected.size()) return false;
  unsigned diff{};
  for (size_t i = 0; i < expected.size(); ++i)
    diff |= static_cast<unsigned char>(supplied[i]) ^ static_cast<unsigned char>(expected[i]);
  return diff == 0;
}

Json rpcResult(const Json& id, Json payload) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(payload)}};
}

Json rpcError(const Json& id, int code, std::string message) {
  return {{"jsonrpc", "2.0"}, {"id", id},
          {"error", {{"code", code}, {"message", std::move(message)}}}};
}

Json toolResult(Json payload, bool error = false) {
  return {{"content", Json::array({{{"type", "text"},
                                   {"text", payload.dump(-1, ' ', false, Json::error_handler_t::replace)}}})},
          {"isError", error}};
}

Json toolError(std::string message) {
  return toolResult({{"message", std::move(message)}}, true);
}

Json tool(std::string_view name, std::string_view description,
          Json properties = Json::object(), Json required = Json::array()) {
  return {{"name", name}, {"description", description},
          {"inputSchema", {{"type", "object"}, {"properties", std::move(properties)},
                           {"required", std::move(required)}, {"additionalProperties", false}}}};
}

const Json& tools() {
  const auto number = [](double minimum, double maximum) {
    return Json{{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}};
  };
  const auto integer = [](uint32_t minimum, uint32_t maximum) {
    return Json{{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
  };
  const Json path{{"type", "string"}, {"maxLength", 1024}};
  const Json measurementId{{"id", integer(1, UINT32_MAX)}};
  const auto lightingProperties = [&] {
    Json properties = Json::object();
    for (const auto& setting : container::app::kRendererLightingSettings)
      properties[std::string(setting.name)] = number(setting.minimum, setting.maximum);
    return properties;
  };
  static const Json result = Json::array({
      tool("renderer_get_settings", "Read the current renderer and live lighting settings."),
      tool("renderer_set_lighting", "Set any of these live lighting controls.",
           lightingProperties()),
      tool("renderer_get_status", "Read model, environment, capture and shader reload status."),
      tool("renderer_recent_logs", "Read a bounded set of redacted renderer and Vulkan events.",
           {{"limit", integer(1, 32)}}),
      tool("renderer_load_model", "Queue a glTF or GLB model from the configured local file root.",
           {{"path", path}, {"importScale", number(.001, 1000)}}, {"path"}),
      tool("renderer_load_environment", "Queue a Radiance HDR or EXR environment from the local file root.",
           {{"path", path}}, {"path"}),
      tool("renderer_reload_shaders", "Start the renderer's asynchronous Slang shader reload."),
      tool("renderer_capture", "Queue a new PNG or EXR in the configured local file root.",
           {{"path", path}, {"viewportOnly", {{"type", "boolean"}}}}, {"path"}),
      tool("renderer_list_gpu_timers", "List active GPU pass timers from the last retired recording."),
      tool("renderer_start_gpu_measurement", "Start a bounded session over distinct retired GPU recordings.",
           {{"timer", {{"type", "string"}, {"maxLength", 128}}}, {"warmupFrames", integer(0, 240)},
            {"samples", integer(1, 240)}, {"timeoutMs", integer(100, 30000)}}, {"timer"}),
      tool("renderer_poll_gpu_measurement", "Read session progress or completed GPU statistics.",
           measurementId, {"id"}),
      tool("renderer_cancel_gpu_measurement", "Cancel the specified GPU measurement.",
           measurementId, {"id"}),
  });
  return result;
}

void validateArguments(const Json& schema, const Json& arguments) {
  if (!arguments.is_object()) throw std::invalid_argument("Tool arguments must be an object");
  for (const auto& required : schema.at("required"))
    if (!arguments.contains(required.get_ref<const std::string&>()))
      throw std::invalid_argument("Missing tool argument: " + required.get<std::string>());
  const auto& properties = schema.at("properties");
  for (const auto& [key, value] : arguments.items()) {
    const auto field = properties.find(key);
    if (field == properties.end()) throw std::invalid_argument("Unknown tool argument: " + key);
    const auto& type = field->at("type");
    if (type == "number" || type == "integer") {
      if (!value.is_number() || (type == "integer" && !value.is_number_integer()))
        throw std::invalid_argument("Expected numeric tool argument: " + key);
      const auto number = value.get<double>();
      if (!std::isfinite(number) || number < field->at("minimum").get<double>() ||
          number > field->at("maximum").get<double>())
        throw std::invalid_argument("Tool argument out of range: " + key);
    } else if (type == "boolean") {
      if (!value.is_boolean()) throw std::invalid_argument("Expected boolean tool argument: " + key);
    } else if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
               value.get_ref<const std::string&>().size() > field->at("maxLength").get<size_t>() ||
               value.get_ref<const std::string&>().find('\0') != std::string::npos) {
      throw std::invalid_argument("Invalid string tool argument: " + key);
    }
  }
}

bool pathInside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
  return std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end()).first == root.end();
}

void validateToolPath(std::string_view name, Json& arguments, const std::filesystem::path& root) {
  if (!arguments.contains("path")) return;
  if (root.empty()) throw std::invalid_argument("Set CONTAINER_MCP_ROOT to enable renderer file tools");
  namespace fs = std::filesystem;
  auto path = container::util::pathFromUtf8(arguments.at("path").get_ref<const std::string&>());
#ifdef _WIN32
  for (const auto& component : path.relative_path())
    if (component.native().find(L':') != std::wstring::npos)
      throw std::invalid_argument("MCP file paths must name ordinary files");
#endif
  path = (root / path).lexically_normal();
  if (!pathInside(root, path)) throw std::invalid_argument("MCP file path is outside its configured root");
  const auto extension = lowercase(container::util::pathToUtf8(path.extension()));
  if (name == "renderer_capture") {
    if (extension != ".png" && extension != ".exr")
      throw std::invalid_argument("MCP capture requires a PNG or EXR path");
    path = fs::canonical(path.parent_path()) / path.filename();
    if (!pathInside(root, path)) throw std::invalid_argument("MCP capture directory resolves outside its root");
    auto metadata = path;
    metadata += ".capture.json";
    auto telemetry = path;
    telemetry.replace_extension(".telemetry.json");
    for (const auto& output : {path, metadata, telemetry})
      if (fs::symlink_status(output).type() != fs::file_type::not_found)
        throw std::invalid_argument("MCP capture requires an unused output path and sidecar names");
  } else {
    if (name == "renderer_load_model" && extension != ".gltf" && extension != ".glb")
      throw std::invalid_argument("MCP model loading requires a glTF or GLB file");
    if (name == "renderer_load_environment" && extension != ".hdr" && extension != ".exr")
      throw std::invalid_argument("MCP environment loading requires an HDR or EXR file");
    path = fs::canonical(path);
    if (!pathInside(root, path) || !fs::is_regular_file(path))
      throw std::invalid_argument("MCP input must resolve to a regular file within its root");
  }
  arguments["path"] = container::util::pathToUtf8(path);
}

std::filesystem::path fileRoot(std::filesystem::path root) {
  if (root.empty()) return {};
  root = std::filesystem::canonical(root);
  if (!std::filesystem::is_directory(root))
    throw std::invalid_argument("CONTAINER_MCP_ROOT must identify a directory");
  return root;
}

std::string bearerToken(std::string value) {
  if (value.size() < 32 || value.size() > 256 ||
      std::ranges::any_of(value, [](unsigned char c) { return c < 0x21 || c > 0x7e; }))
    throw std::invalid_argument("CONTAINER_MCP_TOKEN must contain 32 to 256 printable non-space ASCII characters");
  return value;
}

struct PendingCommand {
  std::string toolName;
  Json arguments;
  Clock::time_point deadline;
  NativeSocket client;
  bool disconnected{};
  std::condition_variable ready;
  std::optional<Json> response;
};

} // namespace

struct RendererMcpServer::State {
  explicit State(uint16_t requestedPort, std::string secret, std::filesystem::path rootPath)
      : token(bearerToken(std::move(secret))), root(fileRoot(std::move(rootPath))),
        listener(makeListener(requestedPort, port)),
        worker([this](std::stop_token stop) { serve(stop); }) {}

  void serve(std::stop_token stop);
  void handle(NativeSocket client, const HttpRequest& request, std::stop_token stop);
  Json queue(NativeSocket client, std::string toolName, Json arguments, std::stop_token stop);
  Json run(std::string_view name, const Json& arguments, const OwnerHandler& handler);

  std::string token;
  std::filesystem::path root;
  NetworkRuntime network;
  uint16_t port{};
  Socket listener;
  // Protects the pending slot, peer checks, and command responses. The worker
  // keeps each client socket open until it has left the queue under this lock.
  std::mutex commandMutex;
  std::shared_ptr<PendingCommand> pending;

  // Only the renderer owner thread reads and writes the following state.
  std::vector<RendererPassGpuTiming> timers;
  std::string timingSource{"none"};
  uint64_t latestSerial{};
  uint32_t sessionId{};
  std::string sessionTimer;
  std::string sessionSource;
  std::optional<RendererMcpMeasurement> measurement;
  bool capturesLogs{false};
  std::jthread worker;
};

void RendererMcpServer::State::serve(std::stop_token stop) {
  while (!stop.stop_requested()) {
    if (!waitForSocket(listener.handle, false, Clock::now() + 100ms, stop)) continue;
    Socket client(accept(listener.handle, nullptr, nullptr));
    if (client.handle == invalidSocket || !makeNonblocking(client.handle)) continue;
#ifdef SO_NOSIGPIPE
    const int suppress = 1;
    setsockopt(client.handle, SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress));
#endif
    try {
      if (const auto request = readRequest(client.handle, stop))
        handle(client.handle, *request, stop);
    } catch (const HttpError& error) {
      sendResponse(client.handle, error.code, rpcError(nullptr, -32600, error.what()).dump(), stop);
    } catch (const std::exception&) {
      sendResponse(client.handle, 400, rpcError(nullptr, -32600, "Invalid MCP request").dump(), stop);
    }
  }
}

void RendererMcpServer::State::handle(NativeSocket client, const HttpRequest& request,
                                      std::stop_token stop) {
  if (request.headers.contains("origin") ||
      request.header("host") != "127.0.0.1:" + std::to_string(port))
    throw HttpError(403, "MCP requires a direct loopback host and no browser Origin");
  if (!authorized(request.header("authorization"), token))
    throw HttpError(401, "MCP requires its configured bearer token");
  if (request.path != "/mcp") throw HttpError(404, "MCP endpoint is /mcp");
  if (request.method != "POST") throw HttpError(405, "This MCP endpoint accepts POST only");
  const auto contentType = request.header("content-type");
  if (lowercase(trim(contentType.substr(0, contentType.find(';')))) != "application/json")
    throw HttpError(400, "MCP POST requires application/json");
  const auto accepted = lowercase(request.header("accept"));
  if (accepted.find("application/json") == std::string::npos ||
      accepted.find("text/event-stream") == std::string::npos)
    throw HttpError(400, "MCP POST must accept application/json and text/event-stream");
  const auto protocol = request.header("mcp-protocol-version");
  if (!protocol.empty() && !supportsProtocol(protocol))
    throw HttpError(400, "Unsupported MCP protocol version");

  const auto respond = [&](const Json& value) {
    sendResponse(client, 200, value.dump(-1, ' ', false, Json::error_handler_t::replace), stop);
  };
  const auto message = Json::parse(request.body, nullptr, false);
  if (message.is_discarded()) {
    respond(rpcError(nullptr, -32700, "Invalid JSON"));
    return;
  }
  if (!message.is_object() || !message.contains("jsonrpc") || message.at("jsonrpc") != "2.0") {
    respond(rpcError(nullptr, -32600, "Expected one JSON-RPC 2.0 message"));
    return;
  }
  const auto method = message.find("method");
  const auto id = message.find("id");
  if (id == message.end()) {
    if (method == message.end() || !method->is_string()) {
      respond(rpcError(nullptr, -32600, "Invalid JSON-RPC notification"));
      return;
    }
    sendResponse(client, 202, {}, stop);
    return;
  }
  if ((!id->is_string() && !id->is_number_integer()) || method == message.end() || !method->is_string()) {
    respond(rpcError(nullptr, -32600, "Invalid JSON-RPC request"));
    return;
  }
  const auto params = message.value("params", Json::object());
  if (!params.is_object()) {
    respond(rpcError(*id, -32602, "MCP params must be an object"));
    return;
  }
  if (*method == "initialize") {
    const auto version = params.find("protocolVersion");
    if (version == params.end() || !version->is_string()) {
      respond(rpcError(*id, -32602, "initialize requires protocolVersion"));
      return;
    }
    const auto requested = version->get<std::string>();
    respond(rpcResult(*id, {{"protocolVersion", supportsProtocol(requested) ? requested : std::string(currentProtocol)},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", "VulkanSceneRenderer"}, {"version", "1.0"}}}}));
  } else if (*method == "ping") {
    respond(rpcResult(*id, Json::object()));
  } else if (*method == "tools/list") {
    respond(rpcResult(*id, {{"tools", tools()}}));
  } else if (*method == "tools/call") {
    const auto name = params.find("name");
    if (name == params.end() || !name->is_string()) {
      respond(rpcError(*id, -32602, "tools/call requires a tool name"));
      return;
    }
    const auto found = std::ranges::find_if(tools(), [&](const Json& entry) { return entry.at("name") == *name; });
    if (found == tools().end()) {
      respond(rpcError(*id, -32602, "Unknown renderer tool"));
      return;
    }
    try {
      auto arguments = params.value("arguments", Json::object());
      validateArguments(found->at("inputSchema"), arguments);
      const auto toolName = name->get<std::string>();
      if (toolName == "renderer_recent_logs") {
        Json entries = Json::array();
        for (const auto& entry : container::log::recentRendererMcpLogs(arguments.value("limit", 16u))) {
          entries.push_back({{"component", entry.component}, {"severity", entry.severity},
                             {"event", entry.event}, {"recordedAtUnixMs", entry.recordedAtUnixMs}});
        }
        respond(rpcResult(*id, toolResult({{"entries", std::move(entries)}})));
        return;
      }
      validateToolPath(toolName, arguments, root);
      respond(rpcResult(*id, queue(client, toolName, std::move(arguments), stop)));
    } catch (const std::exception& error) {
      respond(rpcResult(*id, toolError(error.what())));
    }
  } else {
    respond(rpcError(*id, -32601, "Unknown MCP method"));
  }
}

Json RendererMcpServer::State::queue(NativeSocket client, std::string toolName, Json arguments,
                                    std::stop_token stop) {
  auto command = std::make_shared<PendingCommand>();
  command->toolName = std::move(toolName);
  command->arguments = std::move(arguments);
  command->deadline = Clock::now() + requestTimeout;
  command->client = client;
  std::unique_lock lock(commandMutex);
  pending = command;
  const auto abandon = [&](std::string message) {
    if (pending == command) pending.reset();
    return toolError(std::move(message));
  };
  while (!stop.stop_requested()) {
    if (command->response) return std::move(*command->response);
    if (command->disconnected || peerDisconnected(client))
      return abandon("The MCP connection closed");
    const auto now = Clock::now();
    if (now >= command->deadline) break;
    command->ready.wait_until(lock, std::min(now + 25ms, command->deadline));
  }
  if (stop.stop_requested()) return abandon("The MCP server is shutting down");
  if (pending == command) return abandon("Renderer did not accept the command before the request timed out");
  return toolError("Renderer accepted the command, but its response timed out");
}

Json RendererMcpServer::State::run(std::string_view name, const Json& arguments,
                                  const OwnerHandler& handler) {
  const auto now = Clock::now();
  const bool measuring = measurement && measurement->status(now) == RendererMcpMeasurement::Status::Running;
  if (name == "renderer_list_gpu_timers") {
    Json active = Json::array();
    for (const auto& timer : timers) active.push_back(renderPassName(timer.id));
    return {{"timers", std::move(active)}, {"source", timingSource}, {"recordingSerial", latestSerial}};
  }
  if (name == "renderer_start_gpu_measurement") {
    if (measuring) throw std::invalid_argument("A GPU measurement is already running");
    const auto timer = arguments.at("timer").get<std::string>();
    if (std::ranges::none_of(timers, [&](const auto& entry) { return renderPassName(entry.id) == timer; }))
      throw std::invalid_argument("GPU timer is unavailable in the last retired frame: " + timer);
    if (sessionId == UINT32_MAX) throw std::runtime_error("GPU measurement IDs exhausted");
    measurement.emplace(latestSerial, arguments.value("warmupFrames", 8u),
                        arguments.value("samples", 30u),
                        now + std::chrono::milliseconds(arguments.value("timeoutMs", 10000u)));
    sessionTimer = timer;
    sessionSource = timingSource;
    return {{"id", ++sessionId}, {"timer", sessionTimer}, {"source", sessionSource}, {"status", "running"}};
  }
  if (name == "renderer_poll_gpu_measurement" || name == "renderer_cancel_gpu_measurement") {
    if (!measurement || arguments.at("id").get<uint32_t>() != sessionId)
      throw std::invalid_argument("Unknown GPU measurement ID");
    if (name == "renderer_cancel_gpu_measurement") measurement->cancel(now);
    auto report = measurement->report(now);
    report["id"] = sessionId;
    report["timer"] = sessionTimer;
    report["source"] = sessionSource;
    if (report.at("status") == "timedOut")
      report["reason"] = "The requested number of distinct GPU samples did not retire by the deadline";
    return report;
  }
  if (measuring && name != "renderer_get_status" && name != "renderer_get_settings")
    throw std::invalid_argument("Finish or cancel the active GPU measurement before mutating the renderer");
  auto result = handler(name, arguments);
  if (name == "renderer_get_settings") result["fileToolsAvailable"] = !root.empty();
  return result;
}

std::unique_ptr<RendererMcpServer> RendererMcpServer::startFromEnvironment() {
  const char* value = std::getenv("CONTAINER_MCP_PORT");
  if (!value || !*value) return {};
  const std::string_view text(value);
  uint16_t port{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), port);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || port < 1024)
    throw std::invalid_argument("CONTAINER_MCP_PORT must be an integer between 1024 and 65535");
  const char* token = std::getenv("CONTAINER_MCP_TOKEN");
  const char* root = std::getenv("CONTAINER_MCP_ROOT");
  auto server = std::make_unique<RendererMcpServer>(port, token ? token : "",
      root ? container::util::pathFromUtf8(root) : std::filesystem::path{});
  container::log::setRendererMcpLogCapture(true);
  server->state_->capturesLogs = true;
  return server;
}

RendererMcpServer::RendererMcpServer(uint16_t port, std::string token, std::filesystem::path root)
    : state_(std::make_unique<State>(port, std::move(token), std::move(root))) {}

RendererMcpServer::~RendererMcpServer() {
  const bool capturesLogs = state_->capturesLogs;
  state_.reset();
  if (capturesLogs) container::log::setRendererMcpLogCapture(false);
}

uint16_t RendererMcpServer::port() const { return state_->port; }

void RendererMcpServer::pump(const OwnerHandler& handler) {
  std::shared_ptr<PendingCommand> command;
  {
    const std::unique_lock lock(state_->commandMutex, std::try_to_lock);
    if (!lock.owns_lock() || !state_->pending || state_->pending->disconnected) return;
    auto& pending = state_->pending;
    if (peerDisconnected(pending->client)) {
      pending->disconnected = true;
      pending->ready.notify_one();
      return;
    }
    if (Clock::now() >= pending->deadline) return;
    command = std::exchange(pending, {});
  }
  Json result;
  try {
    result = toolResult(state_->run(command->toolName, command->arguments, handler));
  } catch (const std::exception& error) {
    result = toolError(error.what());
  }
  {
    const std::lock_guard lock(state_->commandMutex);
    command->response = std::move(result);
  }
  command->ready.notify_one();
}

void RendererMcpServer::observeGpuTimings(uint64_t serial, std::string_view source,
                                         std::span<const RendererPassGpuTiming> timings) {
  auto& state = *state_;
  if (!serial) {
    state.timers.clear();
    state.timingSource = source;
    return;
  }
  if (serial <= state.latestSerial || timings.empty()) return;
  state.latestSerial = serial;
  state.timingSource = source;
  state.timers.assign(timings.begin(), timings.end());
  if (!state.measurement) return;

  const auto now = Clock::now();
  if (state.timingSource != state.sessionSource) {
    state.measurement->cancel(now);
    return;
  }
  const auto timer = std::ranges::find_if(timings,
      [&](const auto& entry) { return renderPassName(entry.id) == state.sessionTimer; });
  state.measurement->observe(serial, timer == timings.end() ? std::nullopt :
      std::optional<double>{timer->milliseconds}, now);
}

} // namespace container::renderer
