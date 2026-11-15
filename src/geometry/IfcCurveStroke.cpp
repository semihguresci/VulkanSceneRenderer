#include "IfcCurveStroke.h"

#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include <glm/geometric.hpp>

namespace container::geometry::ifc::detail {
namespace {

constexpr size_t maxPatternPairs = 64;
constexpr size_t maxStrokeVertices = 65'536;
constexpr size_t maxSplitSteps = 4 * maxStrokeVertices;

bool positive(double n) { return std::isfinite(n) && n > 0; }

bool finite(glm::vec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

const StepValue *argument(const Entity &entity, size_t index) {
  return entity.args.kind == StepValue::Kind::List &&
                 index < entity.args.list.size()
             ? &entity.args.list[index]
             : nullptr;
}

bool omitted(const StepValue *value) {
  return !value || value->kind == StepValue::Kind::Omitted;
}

bool byLayer(const StepValue *value) {
  return value && value->kind == StepValue::Kind::String &&
         value->text == "by layer";
}

bool namedByLayer(const Entity *item, std::string_view type) {
  return item && item->type == type && byLayer(argument(*item, 0));
}

std::optional<double> number(const StepValue *value) {
  return value && value->kind == StepValue::Kind::Number &&
                 std::isfinite(value->number)
             ? std::optional(value->number)
             : std::nullopt;
}

std::optional<double> length(const StepValue *value) {
  if (!value || value->kind != StepValue::Kind::List || value->list.size() != 1)
    return std::nullopt;
  if (value->text != "IFCPOSITIVELENGTHMEASURE" &&
      value->text != "IFCLENGTHMEASURE")
    return std::nullopt;
  return number(&value->list.front());
}

const Entity *referenced(const std::unordered_map<uint32_t, Entity> &entities,
                         const StepValue *value) {
  if (!value || value->kind != StepValue::Kind::Ref)
    return nullptr;
  const auto found = entities.find(value->ref);
  return found == entities.end() ? nullptr : &found->second;
}

std::vector<double>
readPattern(const std::unordered_map<uint32_t, Entity> &entities,
            const Entity *font) {
  if (!font)
    return {};
  double factor = 1;
  if (font->type == "IFCCURVESTYLEFONTANDSCALING") {
    const auto scale = number(argument(*font, 2));
    if (!scale || !positive(*scale))
      return {};
    factor = *scale;
    font = referenced(entities, argument(*font, 1));
  }
  // The font select inside FontAndScaling has no further scaling wrapper.
  // Resolve only this finite schema path, including for cyclic input.
  if (!font || font->type != "IFCCURVESTYLEFONT")
    return {};
  const auto *pairs = argument(*font, 1);
  if (!pairs || pairs->kind != StepValue::Kind::List || pairs->list.empty() ||
      pairs->list.size() > maxPatternPairs)
    return {};
  std::vector<double> result;
  result.reserve(2 * pairs->list.size());
  for (const auto &item : pairs->list) {
    const auto *pair = referenced(entities, &item);
    if (!pair || pair->type != "IFCCURVESTYLEFONTPATTERN")
      return {};
    for (size_t field : {0u, 1u}) {
      const auto *raw = argument(*pair, field);
      auto value = number(raw);
      if (!value)
        value = length(raw);
      if (!value)
        return {};
      const double scaled = *value * factor;
      if (!(field == 0 && *value == 0.) && !positive(scaled))
        return {};
      result.push_back(scaled);
    }
  }
  return result;
}

glm::vec3 atDistance(glm::vec3 a, glm::vec3 b, double at, double length) {
  const double fraction = at / length;
  return glm::vec3(std::lerp(double(a.x), double(b.x), fraction),
                   std::lerp(double(a.y), double(b.y), fraction),
                   std::lerp(double(a.z), double(b.z), fraction));
}

} // namespace

IfcCurveStroke
readIfcCurveStroke(const std::unordered_map<uint32_t, Entity> &entities,
                   const Entity &curveStyle) {
  if (curveStyle.type != "IFCCURVESTYLE" ||
      curveStyle.args.kind != StepValue::Kind::List)
    return {};
  const auto *rawFont = argument(curveStyle, 1);
  const auto *rawWidth = argument(curveStyle, 2);
  const auto *rawColor = argument(curveStyle, 3);
  const auto *font = referenced(entities, rawFont);
  IfcCurveStroke result;
  result.patternFromLayer = omitted(rawFont) ||
                            namedByLayer(font, "IFCDRAUGHTINGPREDEFINEDCURVEFONT");
  result.widthFromLayer =
      omitted(rawWidth) ||
      (rawWidth->kind == StepValue::Kind::List &&
       rawWidth->text == "IFCDESCRIPTIVEMEASURE" && rawWidth->list.size() == 1 &&
       byLayer(&rawWidth->list.front()));
  result.colorFromLayer =
      omitted(rawColor) ||
      namedByLayer(referenced(entities, rawColor), "IFCDRAUGHTINGPREDEFINEDCOLOUR");

  const auto *scope = argument(curveStyle, 4);
  if (!omitted(scope) &&
      (scope->kind != StepValue::Kind::Enum || scope->text != "T"))
    return result; // Paper-space dimensions require a plotting scale.
  const double width = length(rawWidth).value_or(0.);
  if (positive(width)) result.width = width;
  result.pattern = readPattern(entities, font);
  return result;
}

std::optional<IfcCurveStrokePaths>
splitIfcCurveStroke(std::span<const glm::vec3> points,
                    std::span<const size_t> counts,
                    std::span<const double> pattern, std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  if (points.size() > maxStrokeVertices || counts.empty() ||
      counts.size() > points.size() || pattern.empty() ||
      pattern.size() > 2 * maxPatternPairs || pattern.size() % 2 != 0)
    return std::nullopt;
  for (size_t i = 0; i < pattern.size(); ++i)
    if (!(i % 2 == 0 && pattern[i] == 0.) && !positive(pattern[i]))
      return std::nullopt;

  IfcCurveStrokePaths result;
  result.points.reserve(points.size());
  size_t consumed = 0, steps = 0, pathStart = 0;
  const auto finishPath = [&] {
    const size_t count = result.points.size() - pathStart;
    if (count >= 2)
      result.counts.push_back(count);
    else
      result.points.resize(pathStart);
    pathStart = result.points.size();
  };
  const auto append = [&](glm::vec3 point) {
    if (result.points.size() > pathStart && result.points.back() == point)
      return true;
    if (result.points.size() + result.dots.size() >= maxStrokeVertices)
      return false;
    result.points.push_back(point);
    return true;
  };

  for (size_t count : counts) {
    if (count == 0 || count > points.size() - consumed ||
        !finite(points[consumed]))
      return std::nullopt;
    size_t slot = 0;
    double available = pattern.front();
    const auto emitDot = [&](glm::vec3 point) {
      if (available != 0)
        return true;
      if (result.dots.size() + result.points.size() >= maxStrokeVertices)
        return false;
      if (result.dots.empty() || result.dots.back() != point)
        result.dots.push_back(point);
      // Zero lengths are valid only on the visible side of a pair.
      available = pattern[++slot];
      return true;
    };
    for (size_t i = 1; i < count; ++i) {
      if (i % 1024 == 0)
        container::util::checkModelImportCancellation(stop);
      const auto a = points[consumed + i - 1], b = points[consumed + i];
      if (!finite(b))
        return std::nullopt;
      const double length = glm::length(glm::dvec3(b) - glm::dvec3(a));
      if (length > 0 && !emitDot(a))
        return std::nullopt;
      double offset = 0;
      while (offset < length) {
        if (++steps > maxSplitSteps)
          return std::nullopt;
        if (steps % 1024 == 0)
          container::util::checkModelImportCancellation(stop);
        const double rest = length - offset;
        const bool endsPattern = available <= rest;
        const double next = available >= rest ? length : offset + available;
        if (next <= offset)
          return std::nullopt;
        if (slot % 2 == 0 && (!append(atDistance(a, b, offset, length)) ||
                              !append(atDistance(a, b, next, length))))
          return std::nullopt;
        offset = next;
        if (endsPattern) {
          if (slot % 2 == 0)
            finishPath();
          slot = (slot + 1) % pattern.size();
          available = pattern[slot];
          if (!emitDot(atDistance(a, b, offset, length)))
            return std::nullopt;
        } else {
          available -= rest;
        }
      }
    }
    finishPath();
    consumed += count;
  }
  return consumed == points.size() &&
                 (!result.counts.empty() || !result.dots.empty())
             ? std::optional(std::move(result))
             : std::nullopt;
}

} // namespace container::geometry::ifc::detail
