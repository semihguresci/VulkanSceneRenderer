#include <Container/geometry/GltfInteractivity.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <variant>

namespace container::geometry::gltf {
namespace {

using Json = tinygltf::Value;
using Numeric = std::vector<double>;
using Value = std::variant<std::monostate, bool, int32_t, Numeric, std::string>;
template<class T> using Sockets = std::map<std::string, T, std::less<>>;
constexpr size_t kMaxNodes = 2048, kMaxTable = 512, kMaxSockets = 64, kMaxText = 512;
constexpr size_t kMaxSteps = 4096, kMaxDepth = 96, kMaxPending = 128;
constexpr double kMaxDuration = 86400.0;

enum class Type { Bool, Int, Float, Float2, Float3, Float4, Ref };
enum class Op {
  Start, Tick, Receive, Send, StopEvent, Select, HoverIn, HoverOut,
  Sequence, Branch, Delay, CancelDelay,
  GetVar, SetVar, InterpolateVar, GetPointer, SetPointer, InterpolatePointer,
  StartClip, StopClip, StopClipAt,
  Add, Subtract, Multiply, Divide, Min, Max, Clamp, Mix, Equal, Less,
  LessEqual, Greater, GreaterEqual, Choose, Combine2, Combine3, Combine4,
  Sin, Cos, Abs, Negate, IsNaN, IntToFloat, FloatToInt, RefEqual,
};
using enum Op;
struct Input { Value literal; std::optional<size_t> node; std::string socket; };
struct Node {
  Op op{};
  std::string name;
  Sockets<Input> inputs;
  Sockets<size_t> flows;
  size_t index{};
  std::vector<size_t> variables;
  Type type{};
  std::string pointer;
};
struct CustomEvent { std::string id; Sockets<Value> defaults; };
struct Graph {
  std::vector<Node> nodes;
  std::vector<Value> variables;
  std::vector<CustomEvent> events;
  std::vector<std::string> eventNames;
  std::map<std::pair<Op, size_t>, std::vector<size_t>> handlers;
};

[[noreturn]] void invalid(std::string message) {
  throw std::invalid_argument(std::move(message));
}
const Json::Object& object(const Json& json) {
  if (!json.IsObject()) invalid("expected an object");
  return json.Get<Json::Object>();
}
const Json& get(const Json& json, std::string_view key) {
  const auto& entries = object(json);
  const auto it = entries.find(std::string(key));
  static const Json absent;
  return it == entries.end() ? absent : it->second;
}
const Json::Array& array(const Json& json, size_t limit, bool optional = false) {
  static const Json::Array empty;
  if (optional && json.Type() == tinygltf::NULL_TYPE) return empty;
  if (!json.IsArray() || json.ArrayLen() > limit) invalid("invalid or oversized array");
  return json.Get<Json::Array>();
}
std::string string(const Json& json) {
  if (!json.IsString()) invalid("expected a string");
  const auto& result = json.Get<std::string>();
  if (result.empty() || result.size() > kMaxText || result.find('\0') != std::string::npos)
    invalid("invalid or oversized string");
  return result;
}
size_t index(const Json& json, size_t limit) {
  if (!json.IsInt() || json.Get<int>() < 0 || size_t(json.Get<int>()) >= limit)
    invalid("index is out of range");
  return size_t(json.Get<int>());
}
size_t width(Type type) {
  if (type < Type::Float || type > Type::Float4) return 0;
  return size_t(type) - size_t(Type::Float) + 1;
}
Type parseType(const Json& json) {
  static constexpr std::string_view names[]{"bool", "int", "float", "float2", "float3", "float4", "ref"};
  const auto signature = string(get(json, "signature"));
  const auto found = std::ranges::find(names, signature);
  if (found == std::end(names)) invalid("unsupported value type '" + signature + "'");
  return static_cast<Type>(found - std::begin(names));
}
Value initialValue(Type type) {
  switch (type) {
    case Type::Bool: return false;
    case Type::Int: return int32_t{0};
    case Type::Ref: return std::string{};
    default: return Numeric(width(type), std::numeric_limits<double>::quiet_NaN());
  }
}
double literalNumber(const Json& json) {
  if (json.IsNumber()) {
    const float value = float(json.GetNumberAsDouble());
    if (std::isfinite(value)) return value;
  } else if (json.IsString()) {
    const auto& token = json.Get<std::string>();
    if (token == "NaN") return std::numeric_limits<double>::quiet_NaN();
    if (token == "Infinity") return std::numeric_limits<double>::infinity();
    if (token == "-Infinity") return -std::numeric_limits<double>::infinity();
  }
  invalid("expected a binary32 numeric literal");
}
Value literal(const Json& json, Type type, bool optional = false) {
  if (optional && json.Type() == tinygltf::NULL_TYPE) return initialValue(type);
  const auto& items = array(json, 4);
  if (items.size() != std::max(size_t{1}, width(type))) invalid("literal has the wrong number of components");
  const auto& first = items.front();
  if (type == Type::Bool) {
    if (first.IsBool()) return first.Get<bool>();
    if (first.IsInt() && (first.Get<int>() == 0 || first.Get<int>() == 1)) return first.Get<int>() != 0;
    invalid("expected a boolean literal");
  }
  if (type == Type::Int) {
    if (!first.IsInt()) invalid("expected an int32 literal");
    return int32_t(first.Get<int>());
  }
  if (type == Type::Ref) return first.Type() == tinygltf::NULL_TYPE ? std::string{} : string(first);
  Numeric result;
  for (const auto& item : items) result.push_back(literalNumber(item));
  return result;
}
bool matches(const Value& a, const Value& b) {
  if (a.index() != b.index()) return false;
  if (const auto* vec = std::get_if<Numeric>(&a)) return vec->size() == std::get<Numeric>(b).size();
  return true;
}
Op operation(std::string_view name, std::string_view extension) {
  if (extension == "KHR_node_selectability" && name == "event/onSelect") return Select;
  if (extension == "KHR_node_hoverability" && name == "event/onHoverIn") return HoverIn;
  if (extension == "KHR_node_hoverability" && name == "event/onHoverOut") return HoverOut;
  static constexpr std::pair<std::string_view, Op> ops[] = {
    {"event/onStart", Start}, {"event/onTick", Tick}, {"event/receive", Receive}, {"event/send", Send},
    {"event/stopPropagation", StopEvent}, {"flow/sequence", Sequence}, {"flow/branch", Branch},
    {"flow/setDelay", Delay}, {"flow/cancelDelay", CancelDelay}, {"variable/get", GetVar},
    {"variable/set", SetVar}, {"variable/interpolate", InterpolateVar}, {"pointer/get", GetPointer},
    {"pointer/set", SetPointer}, {"pointer/interpolate", InterpolatePointer},
    {"animation/start", StartClip}, {"animation/stop", StopClip}, {"animation/stopAt", StopClipAt},
    {"math/add", Add}, {"math/sub", Subtract}, {"math/mul", Multiply}, {"math/div", Divide},
    {"math/min", Min}, {"math/max", Max}, {"math/clamp", Clamp}, {"math/mix", Mix},
    {"math/eq", Equal}, {"math/lt", Less}, {"math/le", LessEqual}, {"math/gt", Greater},
    {"math/ge", GreaterEqual}, {"math/select", Choose}, {"math/combine2", Combine2},
    {"math/combine3", Combine3}, {"math/combine4", Combine4}, {"math/sin", Sin}, {"math/cos", Cos},
    {"math/abs", Abs}, {"math/neg", Negate}, {"math/isNaN", IsNaN}, {"type/intToFloat", IntToFloat},
    {"type/floatToInt", FloatToInt}, {"ref/eq", RefEqual},
  };
  const auto found = std::ranges::find_if(ops, [&](const auto& entry) { return entry.first == name; });
  if (!extension.empty() || found == std::end(ops))
    invalid("unsupported operation '" + std::string(name) + "'" +
        (extension.empty() ? std::string{} : " from " + std::string(extension)));
  return found->second;
}
const Json::Array& configuration(const Json& node, std::string_view name) {
  const auto& value = get(get(get(node, "configuration"), name), "value");
  return array(value, kMaxSockets);
}
const Json& singleConfig(const Json& node, std::string_view name) {
  const auto& values = configuration(node, name);
  if (values.size() != 1) invalid("expected one configuration value for " + std::string(name));
  return values[0];
}
const Json::Object& optionalSockets(const Json& json) {
  static const Json::Object empty;
  if (json.Type() == tinygltf::NULL_TYPE) return empty;
  const auto& entries = object(json);
  if (entries.size() > kMaxSockets) invalid("socket budget exceeded");
  for (const auto& [name, _] : entries)
    if (name.empty() || name.size() > kMaxText) invalid("invalid socket name");
  return entries;
}
Graph compileGraph(const tinygltf::Model& source, const Json& json) {
  Graph result;
  std::vector<Type> types;
  for (const auto& entry : array(get(json, "types"), kMaxTable, true)) types.push_back(parseType(entry));
  for (const auto& entry : array(get(json, "variables"), kMaxTable, true))
    result.variables.push_back(literal(get(entry, "value"), types.at(index(get(entry, "type"), types.size())), true));
  for (const auto& entry : array(get(json, "events"), kMaxTable, true)) {
    CustomEvent event{string(get(entry, "id")), {}};
    if (std::ranges::find(result.eventNames, event.id) != result.eventNames.end())
      invalid("duplicate custom event '" + event.id + "'");
    for (const auto& [name, socket] : optionalSockets(get(entry, "values")))
      event.defaults.emplace(name, literal(get(socket, "value"), types.at(index(get(socket, "type"), types.size())), true));
    result.eventNames.push_back(event.id);
    result.events.push_back(std::move(event));
  }
  const auto& declarations = array(get(json, "declarations"), kMaxTable, true);
  const auto& nodes = array(get(json, "nodes"), kMaxNodes, true);
  for (size_t i = 0; i < nodes.size(); ++i) {
    Node node;
    try {
      const auto& sourceNode = nodes[i];
      const auto& decl = declarations[index(get(sourceNode, "declaration"), declarations.size())];
      const auto& extension = get(decl, "extension");
      node.name = string(get(decl, "op"));
      node.op = operation(node.name, extension.Type() == tinygltf::NULL_TYPE ? "" : string(extension));
      if (node.op == Receive || node.op == Send)
        node.index = index(singleConfig(sourceNode, "event"), result.events.size());
      if (node.op == Select || node.op == HoverIn || node.op == HoverOut)
        node.index = index(singleConfig(sourceNode, "nodeIndex"), source.nodes.size());
      if (node.op == GetVar || node.op == InterpolateVar)
        node.index = index(singleConfig(sourceNode, "variable"), result.variables.size());
      if (node.op == SetVar) {
        for (const auto& id : configuration(sourceNode, "variables"))
          node.variables.push_back(index(id, result.variables.size()));
        if (node.variables.empty()) invalid("variable/set has no destinations");
      }
      if (node.op == GetPointer || node.op == SetPointer || node.op == InterpolatePointer) {
        node.pointer = string(singleConfig(sourceNode, "pointer"));
        node.type = types.at(index(singleConfig(sourceNode, "type"), types.size()));
        if (!node.pointer.starts_with('/') || (node.op != GetPointer && !width(node.type)))
          invalid("unsupported pointer or writable type");
      }
      for (const auto& [name, socket] : optionalSockets(get(sourceNode, "values"))) {
        Input input;
        const auto& ref = get(socket, "node");
        const auto& value = get(socket, "value");
        if (ref.Type() != tinygltf::NULL_TYPE) {
          if (value.Type() != tinygltf::NULL_TYPE) invalid("input combines reference and inline value");
          input.node = index(ref, i);
          const auto& output = get(socket, "socket");
          input.socket = output.Type() == tinygltf::NULL_TYPE ? "value" : string(output);
        } else input.literal = literal(value, types.at(index(get(socket, "type"), types.size())));
        node.inputs.emplace(name, std::move(input));
      }
      for (const auto& [name, flow] : optionalSockets(get(sourceNode, "flows"))) {
        const size_t target = index(get(flow, "node"), nodes.size());
        const auto& socket = get(flow, "socket");
        if (target <= i) invalid("flow must target a later node");
        if (socket.Type() != tinygltf::NULL_TYPE && string(socket) != "in")
          invalid("unsupported flow input socket");
        node.flows.emplace(name, target);
      }
    } catch (const std::invalid_argument& error) {
      invalid("node " + std::to_string(i) + " " + node.name + ": " + error.what());
    }
    if (node.op == Start || node.op == Tick || node.op == Receive ||
        node.op == Select || node.op == HoverIn || node.op == HoverOut)
      result.handlers[{node.op, node.index}].push_back(i);
    result.nodes.push_back(std::move(node));
  }
  return result;
}

std::optional<Graph> compile(const tinygltf::Model& source) {
  const auto found = source.extensions.find(kGltfInteractivityExtension);
  if (found == source.extensions.end()) {
    if (std::ranges::find(source.extensionsRequired, kGltfInteractivityExtension) != source.extensionsRequired.end())
      invalid("KHR_interactivity is required but its root object is missing");
    return {};
  }
  try {
    const auto& graphs = array(get(found->second, "graphs"), 16);
    const auto& requested = get(found->second, "graph");
    const size_t graphIndex = requested.Type() == tinygltf::NULL_TYPE ? 0 : index(requested, graphs.size());
    if (graphs.empty()) invalid("graphs must contain a default graph");
    return compileGraph(source, graphs[graphIndex]);
  } catch (const std::invalid_argument& error) {
    invalid("KHR_interactivity: " + std::string(error.what()));
  }
}

const Numeric& numeric(const Value& value) {
  if (const auto* result = std::get_if<Numeric>(&value)) return *result;
  invalid("expected a floating-point value");
}
double scalar(const Value& value) {
  const auto& values = numeric(value);
  if (values.size() != 1) invalid("expected a scalar");
  return values[0];
}
bool boolean(const Value& value) {
  if (const auto* result = std::get_if<bool>(&value)) return *result;
  invalid("expected a boolean value");
}
std::optional<size_t> referenceIndex(std::string_view reference, std::string_view prefix) {
  if (!reference.starts_with(prefix)) return {};
  reference.remove_prefix(prefix.size());
  if (reference.empty()) return {};
  size_t result{};
  const auto parsed = std::from_chars(reference.data(), reference.data() + reference.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != reference.data() + reference.size()) return {};
  return result;
}
double bezier(double u, double first, double second) {
  const double v = 1.0 - u;
  return 3.0 * v * v * u * first + 3.0 * v * u * u * second + u * u * u;
}
double easing(double progress, const Numeric& p1, const Numeric& p2) {
  if (progress <= 0 || progress >= 1) return progress;
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 24; ++i) {
    const double mid = (lo + hi) * 0.5;
    if (bezier(mid, p1[0], p2[0]) < progress) lo = mid;
    else hi = mid;
  }
  return bezier((lo + hi) * 0.5, p1[1], p2[1]);
}
Numeric interpolate(const Numeric& from, const Numeric& to, double t, bool rotation) {
  if (t == 0) return from;
  if (t == 1) return to;
  if (rotation) {
    const glm::dquat a = glm::normalize(glm::dquat(from[3], from[0], from[1], from[2]));
    const glm::dquat b = glm::normalize(glm::dquat(to[3], to[0], to[1], to[2]));
    const auto q = glm::normalize(glm::slerp(a, b, t));
    return {q.x, q.y, q.z, q.w};
  }
  Numeric result;
  for (size_t i = 0; i < from.size(); ++i) result.push_back(float(std::lerp(from[i], to[i], t)));
  return result;
}
double duration(const Value& value) {
  const double result = scalar(value);
  if (!std::isfinite(result) || result < 0 || result > kMaxDuration) invalid("duration is outside its finite limit");
  return result;
}
double effectiveTime(double requested, double length) {
  if (length <= 0) return 0;
  double time = std::fmod(requested, length);
  if (time < 0) time += length;
  return time == 0 && requested > 0 ? length : time;
}

} // namespace

struct GltfInteractivity::State {
  struct QueuedEvent {
    Op op{};
    size_t customIndex{};
    std::vector<size_t> chain;
    std::optional<Point> point, origin;
  };
  struct Occurrence { std::string ref; bool stopBubble{}, stopImmediate{}; };
  struct DelayState { std::string ref; double deadline{}; size_t owner{}; };
  struct Transition {
    size_t owner{}, variable{};
    std::optional<AnimationTarget> pointer;
    double beginning{}, duration{};
    Numeric from, to, p1, p2;
    bool rotation{};
  };
  struct ClipState {
    size_t index{}, owner{}, work{};
    std::optional<size_t> stopOwner;
    double beginning{}, start{}, end{}, speed{}, stop{};
  };
  struct Frame { tinygltf::Model& model; const AnimationLibrary& library; AnimationDirtySet dirty; };

  Graph graph;
  std::vector<Value> variables;
  std::vector<Sockets<Value>> outputs;
  std::vector<bool> activeHandlers;
  std::vector<QueuedEvent> pending;
  std::vector<DelayState> delays;
  std::vector<Transition> transitions;
  std::vector<ClipState> clips;
  AnimationPose original;
  std::string diagnostic;
  std::vector<Occurrence*> events;
  uint64_t serial{};
  double time{}, delta{};
  size_t budget{}, depth{};
  bool playing{true}, started{};

  explicit State(Graph value) : graph(std::move(value)), variables(graph.variables), outputs(graph.nodes.size()),
      activeHandlers(graph.nodes.size()) {}

  struct Guard {
    explicit Guard(State& state) : state(state) {
      if (state.budget == 0 || state.depth >= kMaxDepth) invalid("per-frame graph work budget exceeded");
      --state.budget;
      ++state.depth;
    }
    ~Guard() { --state.depth; }
    State& state;
  };
  Value input(Frame& frame, const Node& node, std::string_view name, Value fallback = {}) {
    const auto socket = node.inputs.find(name);
    if (socket == node.inputs.end()) return fallback;
    const auto& value = socket->second;
    if (!value.node) return value.literal;
    return evaluate(frame, *value.node, value.socket);
  }
  void flow(Frame& frame, const Node& node, std::string_view name) {
    if (const auto destination = node.flows.find(name); destination != node.flows.end())
      execute(frame, destination->second);
  }
  std::string path(Frame& frame, const Node& node) {
    std::string result;
    std::string_view rest = node.pointer;
    while (!rest.empty()) {
      const size_t begin = rest.find('{');
      if (begin == std::string_view::npos) {
        result += rest;
        break;
      }
      result += rest.substr(0, begin);
      const auto end = rest.find('}', begin + 1);
      if (end == std::string_view::npos || end == begin + 1) invalid("invalid pointer template");
      const auto replacement = input(frame, node, rest.substr(begin + 1, end - begin - 1));
      if (const auto* integer = std::get_if<int32_t>(&replacement); integer && *integer >= 0)
        result += std::to_string(*integer);
      else if (const auto* ref = std::get_if<std::string>(&replacement)) {
        const auto slash = ref->find_last_of('/');
        const auto suffix = slash == std::string::npos ? std::string_view{} : std::string_view(*ref).substr(slash + 1);
        if (!referenceIndex(suffix, "")) invalid("pointer template expects an indexed reference");
        result += suffix;
      } else invalid("pointer template expects a nonnegative index");
      if (result.size() > kMaxText) invalid("resolved pointer is too long");
      rest.remove_prefix(end + 1);
    }
    if (result.size() > kMaxText) invalid("resolved pointer is too long");
    return result;
  }
  AnimationTarget target(Frame& frame, const Node& node) {
    const auto pointer = path(frame, node);
    const auto result = ResolveAnimationPointer(frame.model, pointer);
    if (!result) invalid("unsupported Object Model pointer '" + pointer + "'");
    const auto components = result->element ? 1 : result->components;
    if (components != width(node.type)) invalid("Object Model pointer type does not match");
    return *result;
  }
  void remember(Frame& frame, const AnimationTarget& target) {
    auto whole = target;
    whole.element.reset();
    if (std::ranges::any_of(original.values, [&](const auto& entry) { return entry.target == whole; })) return;
    if (original.values.size() >= kMaxTable || whole.components > kMaxSteps)
      invalid("graph writable-property budget exceeded");
    original.values.push_back({whole, ReadAnimationTarget(frame.model, whole)});
  }
  void publish(Frame& frame, AnimationPose pose) {
    const size_t before = original.values.size();
    try {
      for (const auto& entry : pose.values) {
        if (entry.target.kind == AnimationTargetKind::NodeRotation) {
          double norm = 0;
          for (double component : entry.value) norm += component * component;
          if (!std::isfinite(norm) || norm < 1e-20) invalid("invalid Object Model rotation");
        }
        remember(frame, entry.target);
      }
      frame.dirty.merge(ApplyAnimationPose(frame.model, pose));
    } catch (...) {
      original.values.resize(before);
      throw;
    }
  }
  void write(Frame& frame, AnimationTarget target, Numeric value) {
    if (target.kind == AnimationTargetKind::NodeRotation && value.size() == 4) {
      const glm::dquat q(value[3], value[0], value[1], value[2]);
      if (!std::isfinite(glm::dot(q, q)) || glm::dot(q, q) < 1e-20) invalid("invalid Object Model rotation");
      const auto normalized = glm::normalize(q);
      value = {normalized.x, normalized.y, normalized.z, normalized.w};
    }
    publish(frame, AnimationPose{{{std::move(target), std::move(value)}}});
  }
  void execute(Frame&, size_t);
  Value evaluate(Frame&, size_t, std::string_view);
  void dispatch(Frame&, Op, size_t, Sockets<Value>, const std::vector<size_t>* = nullptr);
  void advancePending(Frame&);
};

Value GltfInteractivity::State::evaluate(Frame& frame, size_t id, std::string_view socket) {
  Guard guard(*this);
  const auto& node = graph.nodes[id];
  if (const auto entry = outputs[id].find(socket); entry != outputs[id].end()) return entry->second;
  if (node.op == GetPointer) {
    auto value = initialValue(node.type);
    bool valid = false;
    try {
      value = ReadAnimationTarget(frame.model, target(frame, node));
      valid = true;
    } catch (const std::invalid_argument&) {
      // Undefined or unsupported reads report isValid=false by specification.
    }
    if (socket == "isValid") return valid;
    if (socket == "value") return value;
  }
  if (socket != "value") invalid("unavailable output '" + std::string(socket) + "' from node " + std::to_string(id));
  if (node.op == GetVar) return variables[node.index];
  const auto arg = [&](std::string_view name) { return input(frame, node, name); };
  if (node.op == Choose) {
    auto a = arg("a");
    auto b = arg("b");
    if (!matches(a, b)) invalid("math/select operands have different types");
    return boolean(arg("condition")) ? std::move(a) : std::move(b);
  }
  if (node.op == Combine2 || node.op == Combine3 || node.op == Combine4) {
    Numeric result{scalar(arg("a")), scalar(arg("b"))};
    if (node.op != Combine2) result.push_back(scalar(arg("c")));
    if (node.op == Combine4) result.push_back(scalar(arg("d")));
    return result;
  }
  const auto a = arg("a");
  if (node.op == IntToFloat) {
    if (const auto* integer = std::get_if<int32_t>(&a)) return Numeric{float(*integer)};
    invalid("intToFloat requires int32");
  }
  if (node.op == FloatToInt) {
    const double v = scalar(a);
    if (!std::isfinite(v)) return int32_t{0};
    double rem = std::fmod(std::trunc(v), 4294967296.0);
    if (rem < 0) rem += 4294967296.0;
    return std::bit_cast<int32_t>(uint32_t(rem));
  }
  if (node.op == IsNaN) return std::isnan(scalar(a));
  if (node.op == Sin || node.op == Cos || node.op == Abs || node.op == Negate) {
    if (const auto* integer = std::get_if<int32_t>(&a); integer && (node.op == Abs || node.op == Negate)) {
      if (node.op == Abs && *integer >= 0) return *integer;
      return std::bit_cast<int32_t>(uint32_t{0} - uint32_t(*integer));
    }
    auto result = numeric(a);
    for (auto& v : result) {
      if (node.op == Sin) v = float(std::sin(v));
      else if (node.op == Cos) v = float(std::cos(v));
      else if (node.op == Abs) v = std::abs(v);
      else v = -v;
    }
    return result;
  }
  const auto b = arg("b");
  if (!matches(a, b) || std::holds_alternative<std::monostate>(a)) invalid("math operands have different types");
  if (node.op == Equal || node.op == RefEqual) return a == b;
  if (const auto* left = std::get_if<int32_t>(&a)) {
    const auto x = *left, y = std::get<int32_t>(b);
    switch (node.op) {
      case Add: return std::bit_cast<int32_t>(uint32_t(x) + uint32_t(y));
      case Subtract: return std::bit_cast<int32_t>(uint32_t(x) - uint32_t(y));
      case Multiply: return std::bit_cast<int32_t>(uint32_t(x) * uint32_t(y));
      case Divide: return y == 0 ? int32_t{0} : y == -1 && x == std::numeric_limits<int32_t>::min() ? x : x / y;
      case Min: return std::min(x, y);
      case Max: return std::max(x, y);
      case Less: return x < y;
      case LessEqual: return x <= y;
      case Greater: return x > y;
      case GreaterEqual: return x >= y;
      default: invalid("unsupported integer math operand");
    }
  }
  if (node.op == Less) return scalar(a) < scalar(b);
  if (node.op == LessEqual) return scalar(a) <= scalar(b);
  if (node.op == Greater) return scalar(a) > scalar(b);
  if (node.op == GreaterEqual) return scalar(a) >= scalar(b);
  auto result = numeric(a);
  const auto& right = numeric(b);
  Numeric third;
  if (node.op == Clamp || node.op == Mix) {
    third = numeric(arg("c"));
    if (third.size() != result.size()) invalid("math operands have different widths");
  }
  for (size_t i = 0; i < result.size(); ++i) {
    const double x = result[i], y = right[i];
    if (std::isnan(x) || std::isnan(y) || (!third.empty() && std::isnan(third[i]))) {
      result[i] = std::numeric_limits<double>::quiet_NaN();
      continue;
    }
    switch (node.op) {
      case Add: result[i] = float(x + y); break;
      case Subtract: result[i] = float(x - y); break;
      case Multiply: result[i] = float(x * y); break;
      case Divide: result[i] = float(x / y); break;
      case Min: result[i] = std::min(x, y); break;
      case Max: result[i] = std::max(x, y); break;
      case Clamp: result[i] = std::min(std::max(x, y), third[i]); break;
      case Mix: result[i] = float(std::lerp(x, y, third[i])); break;
      default: invalid("node does not produce a value");
    }
  }
  return result;
}

void GltfInteractivity::State::dispatch(Frame& frame, Op op, size_t index, Sockets<Value> values,
    const std::vector<size_t>* chain) {
  Guard guard(*this);
  Occurrence event{"#event/" + std::to_string(++serial)};
  values["event"] = event.ref;
  events.push_back(&event);
  const auto activate = [&](size_t key) {
    const auto handlers = graph.handlers.find({op, key});
    if (handlers == graph.handlers.end()) return;
    for (size_t handler : handlers->second) {
      if (event.stopImmediate) break;
      const bool nested = activeHandlers[handler];
      activeHandlers[handler] = true;
      auto previous = std::exchange(outputs[handler], values);
      try { flow(frame, graph.nodes[handler], "out"); }
      catch (...) {
        activeHandlers[handler] = nested;
        if (nested) outputs[handler] = std::move(previous);
        throw;
      }
      activeHandlers[handler] = nested;
      if (nested) outputs[handler] = std::move(previous);
    }
  };
  try {
    if (chain) {
      values[op == Select ? "selectedNode" : "hoveredNode"] = "/nodes/" + std::to_string(chain->front());
      values["controllerIndex"] = int32_t{0};
      for (size_t node : *chain) {
        activate(node);
        if (event.stopBubble) break;
      }
    } else activate(index);
  } catch (...) {
    events.pop_back();
    throw;
  }
  events.pop_back();
}

void GltfInteractivity::State::execute(Frame& frame, size_t id) {
  Guard guard(*this);
  const auto& node = graph.nodes[id];
  const auto arg = [&](std::string_view name, Value fallback = {}) {
    return input(frame, node, name, std::move(fallback));
  };
  std::string_view next = "out";
  try {
    switch (node.op) {
      case Sequence:
        // std::map preserves the required lexicographic output order.
        for (const auto& [name, _] : node.flows) flow(frame, node, name);
        return;
      case Branch:
        next = boolean(arg("condition")) ? "true" : "false";
        break;
      case Delay: {
        if (delays.size() >= kMaxPending) invalid("pending delay budget exceeded");
        const double timeout = duration(arg("duration"));
        std::string ref = "#delay/" + std::to_string(++serial);
        delays.push_back({ref, time + timeout, id});
        outputs[id]["delay"] = std::move(ref);
        break;
      }
      case CancelDelay: {
        const auto value = arg("delay");
        if (const auto* ref = std::get_if<std::string>(&value))
          std::erase_if(delays, [&](const auto& delay) { return delay.ref == *ref; });
        break;
      }
      case StopEvent: {
        const auto value = arg("event");
        const auto* ref = std::get_if<std::string>(&value);
        const bool immediate = boolean(arg("stopImmediate", false));
        if (ref) for (auto* event : events) if (event->ref == *ref) {
          event->stopBubble = true;
          event->stopImmediate |= immediate;
        }
        break;
      }
      case Send: {
        auto values = graph.events[node.index].defaults;
        for (auto& [name, value] : values) {
          auto supplied = arg(name, value);
          if (!matches(value, supplied)) invalid("custom event value has the wrong type");
          value = std::move(supplied);
        }
        dispatch(frame, Receive, node.index, std::move(values));
        break;
      }
      case SetVar: {
        std::vector<Value> values;
        for (const size_t variable : node.variables) {
          auto value = arg(std::to_string(variable));
          if (!matches(value, variables[variable])) invalid("variable/set value has the wrong type");
          values.push_back(std::move(value));
        }
        for (size_t i = 0; i < values.size(); ++i) variables[node.variables[i]] = std::move(values[i]);
        break;
      }
      case SetPointer:
        write(frame, target(frame, node), numeric(arg("value")));
        break;
      case InterpolateVar:
      case InterpolatePointer: {
        Transition entry;
        entry.owner = id;
        entry.beginning = time;
        entry.duration = duration(arg("duration"));
        if (node.op == InterpolatePointer) {
          entry.pointer = target(frame, node);
          entry.from = ReadAnimationTarget(frame.model, *entry.pointer);
          entry.rotation = entry.pointer->kind == AnimationTargetKind::NodeRotation;
        } else {
          entry.variable = node.index;
          entry.from = numeric(variables[entry.variable]);
        }
        entry.to = numeric(arg("value"));
        entry.p1 = numeric(arg("p1", Numeric{0, 0}));
        entry.p2 = numeric(arg("p2", Numeric{1, 1}));
        const auto validControl = [](const Numeric& p) {
          return p.size() == 2 && p[0] >= 0 && p[0] <= 1 && std::isfinite(p[1]);
        };
        const auto finite = [](double v) { return std::isfinite(v); };
        if (entry.to.size() != entry.from.size() || !validControl(entry.p1) || !validControl(entry.p2) ||
            !std::ranges::all_of(entry.to, finite) || !std::ranges::all_of(entry.from, finite))
          invalid("invalid interpolation type, endpoint, or control point");
        if (entry.rotation) {
          double norm = 0;
          for (double value : entry.to) norm += value * value;
          if (!std::isfinite(norm) || norm < 1e-20) invalid("invalid interpolation rotation");
        }
        std::erase_if(transitions, [&](const auto& current) {
          return current.pointer == entry.pointer && (entry.pointer || current.variable == entry.variable);
        });
        if (transitions.size() >= kMaxPending) invalid("pending interpolation budget exceeded");
        transitions.push_back(std::move(entry));
        break;
      }
      case StartClip:
      case StopClip:
      case StopClipAt: {
        const auto value = arg("animation");
        const auto* ref = std::get_if<std::string>(&value);
        const auto clip = ref ? referenceIndex(*ref, "/animations/") : std::nullopt;
        if (!clip || *clip >= frame.library.clips.size() || frame.library.clips[*clip].tracks.empty())
          invalid("invalid or unsupported clip reference");
        const auto active = std::ranges::find(clips, *clip, &ClipState::index);
        if (node.op == StartClip) {
          const double start = scalar(arg("startTime", Numeric{0}));
          const double end = scalar(arg("endTime", Numeric{frame.library.clips[*clip].duration}));
          const double speed = scalar(arg("speed", Numeric{1}));
          if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(speed) ||
              std::abs(start) > kMaxDuration || std::abs(end) > kMaxDuration || speed <= 0)
            invalid("invalid clip time or speed");
          size_t work = 1;
          for (const auto& track : frame.library.clips[*clip].tracks) {
            const size_t components = track.target.element ? 1 : track.target.components;
            if (components >= kMaxSteps - work) invalid("animation clip exceeds the per-frame graph work budget");
            work += components + 1;
          }
          ClipState entry{*clip, id, work, {}, time, start, end, speed, end};
          if (active != clips.end()) *active = entry;
          else {
            if (clips.size() >= kMaxPending) invalid("active clip budget exceeded");
            clips.push_back(entry);
          }
        } else if (node.op == StopClip) {
          if (active != clips.end()) clips.erase(active);
        } else if (active != clips.end()) {
          const double stop = scalar(arg("stopTime"));
          if (!std::isfinite(stop)) invalid("invalid clip stop time");
          active->stop = std::clamp(stop, std::min(active->start, active->end), std::max(active->start, active->end));
          active->stopOwner = id;
        }
        break;
      }
      default: invalid("flow cannot activate this operation");
    }
  } catch (const std::invalid_argument& error) {
    if (!node.flows.contains("err") || budget == 0 || depth >= kMaxDepth)
      invalid("node " + std::to_string(id) + " " + node.name + ": " + error.what());
    next = "err";
  }
  flow(frame, node, next);
}

void GltfInteractivity::State::advancePending(Frame& frame) {
  std::vector<std::string> due;
  for (const auto& delay : delays) if (delay.deadline <= time) due.push_back(delay.ref);
  for (const auto& ref : due) {
    const auto ready = std::ranges::find(delays, ref, &DelayState::ref);
    if (ready == delays.end()) continue;
    const size_t owner = ready->owner;
    delays.erase(ready);
    flow(frame, graph.nodes[owner], "done");
  }

  std::vector<size_t> completed;
  for (auto it = transitions.begin(); it != transitions.end();) {
    Guard guard(*this);
    const auto& entry = *it;
    const double progress = entry.duration == 0 ? 1.0 : std::min(1.0, (time - entry.beginning) / entry.duration);
    auto value = interpolate(entry.from, entry.to, easing(progress, entry.p1, entry.p2), entry.rotation);
    if (entry.pointer) write(frame, *entry.pointer, std::move(value));
    else variables[entry.variable] = std::move(value);
    if (progress < 1) ++it;
    else {
      completed.push_back(entry.owner);
      it = transitions.erase(it);
    }
  }
  for (size_t owner : completed) flow(frame, graph.nodes[owner], "done");

  completed.clear();
  for (auto it = clips.begin(); it != clips.end();) {
    Guard guard(*this);
    const auto& entry = *it;
    if (entry.work >= budget) {
      budget = 0;
      invalid("animation clips exceed the per-frame graph work budget");
    }
    budget -= entry.work;
    const auto& clip = frame.library.clips.at(entry.index);
    const double distance = (time - entry.beginning) * entry.speed;
    const bool reverse = entry.start > entry.end;
    const bool stopped = distance >= std::abs(entry.stop - entry.start);
    const double requested = stopped ? entry.stop : entry.start + (reverse ? -distance : distance);
    publish(frame, SampleAnimation(clip, effectiveTime(requested, clip.duration)));
    if (!stopped) ++it;
    else {
      completed.push_back(entry.stopOwner.value_or(entry.owner));
      it = clips.erase(it);
    }
  }
  for (size_t owner : completed) flow(frame, graph.nodes[owner], "done");
}

GltfInteractivity::GltfInteractivity(const tinygltf::Model& source) {
  if (auto graph = compile(source)) state_ = std::make_unique<State>(std::move(*graph));
}
GltfInteractivity::~GltfInteractivity() = default;
bool GltfInteractivity::available() const { return bool(state_); }
bool GltfInteractivity::playing() const { return state_ && state_->playing; }
bool GltfInteractivity::started() const { return state_ && state_->started; }
double GltfInteractivity::time() const { return state_ ? state_->time : 0; }
size_t GltfInteractivity::activeClips() const { return state_ ? state_->clips.size() : 0; }
const std::string& GltfInteractivity::diagnostic() const {
  static const std::string empty;
  return state_ ? state_->diagnostic : empty;
}
const std::vector<std::string>& GltfInteractivity::customEvents() const {
  static const std::vector<std::string> empty;
  return state_ ? state_->graph.eventNames : empty;
}
void GltfInteractivity::setPlaying(bool playing) {
  if (!state_) return;
  state_->playing = playing;
  if (!playing) state_->pending.clear();
}
bool GltfInteractivity::queueCustomEvent(std::string_view id) {
  if (!playing() || state_->pending.size() >= kMaxPending) return false;
  const auto& names = state_->graph.eventNames;
  const auto found = std::ranges::find(names, id);
  if (found == names.end()) return false;
  state_->pending.push_back({Receive, size_t(found - names.begin())});
  return true;
}
bool GltfInteractivity::queueNodeEvent(NodeEvent event, std::vector<size_t> chain,
    std::optional<Point> point, std::optional<Point> rayOrigin) {
  if (!playing() || chain.empty() || chain.size() > kMaxDepth || state_->pending.size() >= kMaxPending) return false;
  const Op op = event == NodeEvent::Select ? Select : event == NodeEvent::HoverIn ? HoverIn : HoverOut;
  state_->pending.push_back({op, 0, std::move(chain), point, rayOrigin});
  return true;
}
AnimationDirtySet GltfInteractivity::advance(tinygltf::Model& model, const AnimationLibrary& library, double deltaSeconds) {
  if (!playing()) return {};
  auto& state = *state_;
  State::Frame frame{model, library, {}};
  state.budget = kMaxSteps;
  try {
    if (!std::isfinite(state.time + deltaSeconds) || deltaSeconds < 0)
      invalid("invalid interactivity frame delta");
    state.delta = state.started ? deltaSeconds : std::numeric_limits<double>::quiet_NaN();
    if (state.started) state.time += deltaSeconds;
    else {
      state.started = true;
      state.dispatch(frame, Start, 0, {});
    }
    for (const auto& event : std::exchange(state.pending, {})) {
      if (event.op == Receive) {
        state.dispatch(frame, Receive, event.customIndex, state.graph.events[event.customIndex].defaults);
        continue;
      }
      const auto asValue = [](const std::optional<Point>& p) {
        return p ? Numeric(p->begin(), p->end()) : std::get<Numeric>(initialValue(Type::Float3));
      };
      Sockets<Value> values{
        {"selectionPoint", asValue(event.point)}, {"selectionRayOrigin", asValue(event.origin)},
      };
      state.dispatch(frame, event.op, 0, std::move(values), &event.chain);
    }
    state.advancePending(frame);
    state.dispatch(frame, Tick, 0, {{"timeSinceStart", Numeric{float(state.time)}},
        {"timeSinceLastTick", Numeric{float(state.delta)}}});
    state.diagnostic.clear();
  } catch (const std::exception& error) {
    state.diagnostic = error.what();
    setPlaying(false);
  }
  return std::move(frame.dirty);
}
AnimationDirtySet GltfInteractivity::reset(tinygltf::Model& model) {
  if (!state_) return {};
  auto& state = *state_;
  auto dirty = ApplyAnimationPose(model, state.original);
  state.original.values.clear();
  state.variables = state.graph.variables;
  for (auto& values : state.outputs) values.clear();
  state.pending.clear();
  state.delays.clear();
  state.transitions.clear();
  state.clips.clear();
  state.diagnostic.clear();
  state.time = 0;
  state.started = false;
  return dirty;
}
void ValidateGltfInteractivity(const tinygltf::Model& source) {
  (void)compile(source);
}

namespace {
using Domain = InteractivityIndexDomain;
struct ObjectPath {
  Domain domain;
  std::string_view prefix, id, suffix;
};
std::optional<ObjectPath> objectPath(std::string_view path) {
  static constexpr std::pair<std::string_view, Domain> prefixes[]{
      {"/nodes/", Domain::Node}, {"/materials/", Domain::Material},
      {"/extensions/KHR_lights_punctual/lights/", Domain::Light},
      {"/cameras/", Domain::Camera}, {"/animations/", Domain::Animation}};
  for (const auto& [prefix, table] : prefixes) {
    if (!path.starts_with(prefix)) continue;
    const auto tail = path.substr(prefix.size());
    const auto slash = tail.find('/');
    const auto suffix = slash == std::string_view::npos ? std::string_view{} : tail.substr(slash);
    const bool weights = suffix == "/weights" || suffix.starts_with("/weights/");
    return ObjectPath{table == Domain::Node && weights ? Domain::NodeWeights : table,
        prefix, tail.substr(0, slash), suffix};
  }
  return {};
}
bool placeholder(std::string_view token) {
  return token.size() > 2 && token.front() == '{' && token.back() == '}' &&
      token.substr(1, token.size() - 2).find_first_of("{}") == std::string_view::npos;
}
size_t indexCount(const tinygltf::Model& source, Domain domain) {
  switch (domain) {
    case Domain::Node:
    case Domain::NodeWeights: return source.nodes.size();
    case Domain::Material: return source.materials.size();
    case Domain::Light: return source.lights.size();
    case Domain::Camera: return source.cameras.size();
    case Domain::Animation: return source.animations.size();
    default: invalid("invalid interactivity index domain");
  }
}
size_t remappedIndex(const tinygltf::Model& source, const GltfInteractivityMappings& mappings,
    Domain domain, size_t index) {
  const auto& map = mappings[size_t(domain)];
  if (index >= indexCount(source, domain) ||
      (!map.oldToNew.empty() && index >= map.oldToNew.size()))
    invalid("reference index is outside its original graph domain");
  if (map.oldToNew.empty()) {
    if (index < map.destinationSize) return index;
  } else if (map.oldToNew[index] >= 0 && size_t(map.oldToNew[index]) < map.destinationSize)
    return size_t(map.oldToNew[index]);
  invalid("graph reference has no unique retained destination");
}
bool unchangedDomain(const tinygltf::Model& source, const GltfInteractivityMappings& mappings, Domain domain) {
  const auto& map = mappings[size_t(domain)];
  const auto count = indexCount(source, domain);
  if (count != map.destinationSize) return false;
  if (map.oldToNew.empty()) return true;
  if (map.oldToNew.size() != count) return false;
  for (size_t i = 0; i < count; ++i) if (map.oldToNew[i] < 0 || size_t(map.oldToNew[i]) != i) return false;
  return true;
}
std::string remappedPath(const tinygltf::Model& source, const GltfInteractivityMappings& mappings,
    std::array<std::optional<bool>, size_t(Domain::Count)>& unchanged,
    const std::string& text, bool isPointer) {
  const auto path = objectPath(text);
  if (!path) invalid("unsupported graph reference domain '" + text + "'");
  if (isPointer && placeholder(path->id)) {
    auto& same = unchanged[size_t(path->domain)];
    if (!same) same = unchangedDomain(source, mappings, path->domain);
    if (!*same)
      invalid("dynamic pointer crosses a changed index domain '" + text + "'");
    return text;
  }
  const auto index = referenceIndex(path->id, "");
  if (!index || (path->id.size() > 1 && path->id.starts_with('0')))
    invalid("invalid graph object index in '" + text + "'");
  return std::string(path->prefix) + std::to_string(remappedIndex(source, mappings, path->domain, *index)) +
      std::string(path->suffix);
}
const Json::Array& graphTable(const Json& extension) {
  const auto& graphs = array(get(extension, "graphs"), 16);
  if (graphs.empty()) invalid("graphs must contain a default graph");
  const auto& selected = get(extension, "graph");
  if (selected.Type() != tinygltf::NULL_TYPE) (void)index(selected, graphs.size());
  return graphs;
}
bool samePointerTable(const tinygltf::Model& source, const tinygltf::Model& projected, Domain domain) {
  switch (domain) {
    case Domain::Node: return source.nodes == projected.nodes;
    case Domain::NodeWeights: return source.nodes == projected.nodes && source.meshes == projected.meshes;
    case Domain::Material: return source.materials == projected.materials;
    case Domain::Light: return source.lights == projected.lights;
    case Domain::Camera: return source.cameras == projected.cameras;
    case Domain::Animation: return source.animations == projected.animations;
    default: return false;
  }
}
std::optional<AnimationTarget> readablePointer(const tinygltf::Model& model, const std::string& pointer) {
  try {
    auto target = ResolveAnimationPointer(model, pointer);
    if (!target) return {};
    (void)ReadAnimationTarget(model, *target);
    target->index = 0; // Compare the property rather than the remapped ID.
    return target;
  } catch (const std::invalid_argument&) {
    return {};
  }
}
}

tinygltf::Value RemapGltfInteractivity(const tinygltf::Model& source, const GltfInteractivityMappings& mappings,
    const tinygltf::Model* projected) try {
  const auto found = source.extensions.find(kGltfInteractivityExtension);
  if (found == source.extensions.end()) return {};
  (void)graphTable(found->second);
  auto result = found->second;
  std::array<std::optional<bool>, size_t(Domain::Count)> unchanged;
  std::array<std::optional<bool>, size_t(Domain::Count)> unchangedTables;
  auto& graphs = result.Get<Json::Object>().at("graphs").Get<Json::Array>();
  for (auto& graph : graphs) {
    const auto compiled = compileGraph(source, graph);
    const auto& types = array(get(graph, "types"), kMaxTable, true);
    const bool dynamicPointer = std::ranges::any_of(compiled.nodes, [](const auto& node) {
      return node.pointer.find('{') != std::string::npos;
    });
    const auto remapLiteral = [&](Json& record) {
      auto& fields = record.Get<Json::Object>();
      const auto value = fields.find("value"), type = fields.find("type");
      if (value == fields.end() || !value->second.IsArray() || type == fields.end() ||
          parseType(types.at(index(type->second, types.size()))) != Type::Ref) return;
      auto& first = value->second.Get<Json::Array>().front();
      if (first.IsString()) {
        const auto& original = first.Get<std::string>();
        auto remapped = remappedPath(source, mappings, unchanged, original, false);
        // A reference can feed any integer placeholder, including a different
        // Object Model domain. Preserve its numeric meaning or reject.
        if (dynamicPointer && remapped != original)
          invalid("dynamic pointer may consume a remapped reference '" + original + "'");
        first = Json(std::move(remapped));
      }
    };
    auto& fields = graph.Get<Json::Object>();
    if (auto vars = fields.find("variables"); vars != fields.end() && vars->second.IsArray())
      for (auto& variable : vars->second.Get<Json::Array>()) remapLiteral(variable);
    if (auto events = fields.find("events"); events != fields.end() && events->second.IsArray())
      for (auto& event : events->second.Get<Json::Array>())
        if (auto values = event.Get<Json::Object>().find("values");
            values != event.Get<Json::Object>().end() && values->second.IsObject())
          for (auto& [name, value] : values->second.Get<Json::Object>()) remapLiteral(value);
    const auto nodes = fields.find("nodes");
    if (nodes == fields.end() || !nodes->second.IsArray()) continue;
    auto& entries = nodes->second.Get<Json::Array>();
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto& node = compiled.nodes[i];
      auto& entry = entries[i].Get<Json::Object>();
      if (!node.pointer.empty()) {
        auto& config = entry.at("configuration").Get<Json::Object>();
        auto& pointer = config.at("pointer").Get<Json::Object>().at("value").Get<Json::Array>().front();
        const auto remapped = remappedPath(source, mappings, unchanged, node.pointer, true);
        if (projected) {
          if (node.pointer.find('{') == std::string::npos) {
            if (readablePointer(source, node.pointer) != readablePointer(*projected, remapped))
              invalid("graph pointer changes property availability after scene projection '" + node.pointer + "'");
          } else {
            const auto domain = objectPath(node.pointer)->domain;
            auto& same = unchangedTables[size_t(domain)];
            if (!same) same = samePointerTable(source, *projected, domain);
            if (!*same || node.pointer != remapped)
              invalid("dynamic pointer crosses a changed object table '" + node.pointer + "'");
          }
        }
        pointer = Json(remapped);
      }
      if (node.op == Select || node.op == HoverIn || node.op == HoverOut) {
        if (dynamicPointer) {
          auto& same = unchanged[size_t(Domain::Node)];
          if (!same) same = unchangedDomain(source, mappings, Domain::Node);
          if (!*same) invalid("dynamic pointer may consume a remapped picked-node reference");
        }
        auto& config = entry.at("configuration").Get<Json::Object>();
        auto& sourceIndex = config.at("nodeIndex").Get<Json::Object>().at("value").Get<Json::Array>().front();
        const auto destination = remappedIndex(source, mappings, Domain::Node, node.index);
        if (destination > size_t(std::numeric_limits<int>::max())) invalid("node event index exceeds the glTF integer range");
        sourceIndex = Json(int(destination));
      }
      if (auto values = entry.find("values"); values != entry.end() && values->second.IsObject())
        for (auto& [name, value] : values->second.Get<Json::Object>()) remapLiteral(value);
    }
  }
  return result;
} catch (const std::invalid_argument& error) {
  invalid("KHR_interactivity remap: " + std::string(error.what()));
}

std::vector<size_t> GltfInteractivityTransformNodes(const tinygltf::Model& source) {
  const auto found = source.extensions.find(kGltfInteractivityExtension);
  if (found == source.extensions.end()) return {};
  std::vector<size_t> result;
  bool allTransforms = false;
  for (const auto& graph : graphTable(found->second)) {
    for (const auto& node : compileGraph(source, graph).nodes) {
      const auto path = objectPath(node.pointer);
      if (!path || path->domain != Domain::Node) continue;
      const auto component = path->suffix.find('/', 1);
      const auto property = path->suffix.substr(0, component);
      if (property != "/translation" && property != "/rotation" && property != "/scale") continue;
      if (placeholder(path->id)) allTransforms = true;
      else if (const auto i = referenceIndex(path->id, ""); i && *i < source.nodes.size() && source.nodes[*i].matrix.empty())
        result.push_back(*i);
    }
  }
  if (allTransforms) {
    result.clear();
    for (size_t i = 0; i < source.nodes.size(); ++i)
      if (source.nodes[i].matrix.empty()) result.push_back(i);
    return result;
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::optional<std::vector<size_t>> GltfInteractivityAnimationClips(const tinygltf::Model& source) {
  std::vector<size_t> result;
  const auto found = source.extensions.find(kGltfInteractivityExtension);
  if (found == source.extensions.end()) return result;
  for (const auto& graph : graphTable(found->second)) {
    for (const auto& node : compileGraph(source, graph).nodes) {
      if (node.op != StartClip) continue;
      const auto input = node.inputs.find("animation");
      if (input == node.inputs.end()) continue;
      if (input->second.node) return std::nullopt;
      const auto* ref = std::get_if<std::string>(&input->second.literal);
      if (!ref) continue;
      const auto clip = referenceIndex(*ref, "/animations/");
      if (clip && *clip < source.animations.size()) result.push_back(*clip);
    }
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

} // namespace container::geometry::gltf
