#include "IfcSectionedTagRows.h"

#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

#include <glm/geometric.hpp>

namespace container::geometry::ifc::detail {
namespace {

constexpr size_t maxRows = 128;
constexpr size_t maxColumns = 4097;
constexpr size_t maxPoints = 65536;
constexpr size_t maxChecks = 8'000'000;
constexpr size_t absent = std::numeric_limits<size_t>::max();
constexpr double roundoff = 64 * std::numeric_limits<double>::epsilon();

struct Run {
  size_t tag, first, last;
};

struct Row {
  uint32_t profileId;
  std::vector<glm::dvec3> points;
  std::vector<size_t> clusters;
  std::vector<Run> runs;
  std::vector<size_t> runByTag;
};

struct Tag {
  const std::string *name;
  std::set<double> fractions{0};
  size_t present = 0;
};

struct Anchor {
  size_t tag = absent;
  double fraction = 0;

  bool operator==(const Anchor &) const = default;
};

bool finite(glm::dvec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// The STEP geometry readers preserve duplicate points at zero-width corners.
// Use a separate double-roundoff check to avoid turning a short real segment
// into a branch tip merely because it is smaller than the length tolerance.
bool coincident(glm::dvec3 a, glm::dvec3 b) {
  const double scale = std::max({std::abs(a.x), std::abs(a.y), std::abs(a.z),
                                 std::abs(b.x), std::abs(b.y), std::abs(b.z)});
  const auto d = a - b;
  return std::max({std::abs(d.x), std::abs(d.y), std::abs(d.z)}) <=
         scale * roundoff;
}

std::string profile(const Row &row) {
  return "profile #" + std::to_string(row.profileId);
}

[[noreturn]] void reject(const Row &row, const std::string &reason) {
  throw std::runtime_error("Sectioned " + profile(row) + ": " + reason);
}

glm::dvec3 atFraction(const Row &row, const Run &run, double fraction) {
  const double occurrence = fraction * (run.last - run.first);
  const double integer = std::round(occurrence);
  if (std::abs(occurrence - integer) <= roundoff * std::max(1., occurrence))
    return row.points[run.first + size_t(integer)];
  const auto i = size_t(occurrence);
  const auto &a = row.points[run.first + i];
  const auto &b = row.points[run.first + i + 1];
  return a + (b - a) * (occurrence - i);
}

class Alignment {
public:
  Alignment(double tolerance, std::stop_token stop)
      : tolerance_(tolerance), stop_(stop) {}

  IfcSectionTagLayout run(std::span<const IfcSectionTagRow> sections) {
    container::util::checkModelImportCancellation(stop_);
    if (sections.size() < 2 || sections.size() > maxRows ||
        !std::isfinite(tolerance_) || tolerance_ <= 0)
      throw std::runtime_error("Sectioned tags require 2 to 128 rows and a "
                               "positive source tolerance");
    if (sections.front().tags.empty())
      throw std::runtime_error("Sectioned profile #" +
                               std::to_string(sections.front().profileId) +
                               " has no tags");
    const auto &first = sections.front().tags.front();
    const auto &last = sections.front().tags.back();
    if (first == last)
      throw std::runtime_error("Sectioned profile #" +
                               std::to_string(sections.front().profileId) +
                               " requires distinct boundary tags");
    rows_.reserve(sections.size());
    for (const auto &section : sections)
      readRow(section, first, last);
    for (auto &row : rows_)
      row.runByTag.resize(tags_.size(), absent);
    const auto order = columnOrder();
    validateReordering(order);
    const auto anchors = completeMissingRuns();
    return assemble(order, anchors);
  }

private:
  void readRow(const IfcSectionTagRow &input, const std::string &first,
               const std::string &last) {
    container::util::checkModelImportCancellation(stop_);
    auto &row = rows_.emplace_back(Row{input.profileId});
    if (input.tags.size() < 2 || input.tags.size() > maxColumns ||
        input.tags.size() != input.points.size() ||
        input.points.size() > maxPoints - sourcePoints_)
      reject(row, "section tag/point counts differ or exceed their budget");
    sourcePoints_ += input.points.size();
    if (!std::ranges::all_of(input.points, finite))
      reject(row, "contains a nonfinite tagged point");
    const bool reversed =
        input.tags.front() == last && input.tags.back() == first;
    if (!reversed && (input.tags.front() != first || input.tags.back() != last))
      reject(row, "boundary tags differ from " + profile(rows_.front()));
    row.points = input.points;
    if (reversed)
      std::ranges::reverse(row.points);
    if (!(glm::length(row.points.back() - row.points.front()) > tolerance_))
      reject(row, "tagged cross section does not have distinct endpoints");
    const auto orientation = [&](const Row &target) {
      const auto delta = target.points.back() - target.points.front();
      return std::abs(delta.x) > tolerance_ ? delta.x : 0.;
    };
    const double initialDirection = orientation(rows_.front());
    const double rowDirection = orientation(row);
    if (initialDirection != 0 && rowDirection != 0 &&
        std::signbit(initialDirection) != std::signbit(rowDirection))
      reject(row, "boundary orientation folds relative to " +
                      profile(rows_.front()));
    row.clusters.reserve(row.points.size());
    size_t clusterStart = 0;
    size_t group = 0;
    for (size_t i = 0; i < row.points.size(); ++i) {
      if (!coincident(row.points[clusterStart], row.points[i])) {
        clusterStart = i;
        ++group;
      }
      row.clusters.push_back(group);
    }
    for (size_t i = 0; i < row.points.size(); ++i) {
      const auto &name = input.tags[reversed ? row.points.size() - 1 - i : i];
      if (name.empty())
        reject(row, "has an empty section tag");
      const auto [tag, inserted] = tagsByName_.try_emplace(name, tags_.size());
      if (inserted) {
        if (tags_.size() == maxColumns)
          reject(row, "section tags exceed their column budget");
        tags_.push_back(Tag{&tag->first});
      }
      const size_t index = tag->second;
      if (!row.runs.empty() && row.runs.back().tag == index) {
        row.runs.back().last = i;
      } else {
        if (row.runByTag.size() <= index)
          row.runByTag.resize(index + 1, absent);
        if (row.runByTag[index] != absent)
          reject(row, "crossing or repeated nonconsecutive section tag '" +
                          name + "'");
        row.runByTag[index] = row.runs.size();
        row.runs.push_back({index, i, i});
      }
    }
    for (const auto &run : row.runs) {
      auto &tag = tags_[run.tag];
      ++tag.present;
      const auto count = run.last - run.first;
      for (size_t k = 1; k <= count; ++k)
        tag.fractions.insert(double(k) / count);
    }
  }

  std::vector<size_t> columnOrder() const {
    // Keep the original ordering for unchanged tag runs, including two
    // distinct tags deliberately shown on the same zero-width profile point.
    const auto &first = rows_.front().runs;
    if (std::ranges::all_of(rows_, [&](const Row &row) {
          return std::ranges::equal(row.runs, first, {}, &Run::tag, &Run::tag);
        })) {
      std::vector<size_t> order;
      for (const auto &run : first)
        order.push_back(run.tag);
      return order;
    }

    // Bracket each spatial cluster with two auxiliary graph nodes. A collapsed
    // run comes between its cluster's boundaries; a run spanning more than
    // one cluster comes after the first cluster and before its final cluster.
    // This orders every disjoint geometry span without ordering coincident
    // tags and keeps construction linear in the authored point count.
    std::vector<std::vector<size_t>> outgoing(tags_.size());
    std::vector<size_t> incoming(tags_.size());
    for (const auto &row : rows_) {
      container::util::checkModelImportCancellation(stop_);
      const size_t boundary = outgoing.size();
      const size_t boundaryCount = 2 * (row.clusters.back() + 1);
      outgoing.resize(boundary + boundaryCount);
      incoming.resize(outgoing.size());
      const auto link = [&](size_t from, size_t to) {
        outgoing[from].push_back(to);
        ++incoming[to];
      };
      for (size_t i = 1; i < boundaryCount; ++i)
        link(boundary + i - 1, boundary + i);
      for (const auto &run : row.runs) {
        const size_t a = row.clusters[run.first], b = row.clusters[run.last];
        const bool collapsed = a == b;
        link(boundary + 2 * a + (collapsed ? 0 : 1), run.tag);
        link(run.tag, boundary + 2 * b + (collapsed ? 1 : 0));
      }
    }
    std::set<size_t> readyTags;
    std::vector<size_t> readyBoundaries;
    const auto ready = [&](size_t node) {
      if (node < tags_.size())
        readyTags.insert(node);
      else
        readyBoundaries.push_back(node);
    };
    for (size_t i = 0; i < incoming.size(); ++i)
      if (incoming[i] == 0)
        ready(i);
    const auto visit = [&](size_t node) {
      for (const size_t next : outgoing[node])
        if (--incoming[next] == 0)
          ready(next);
    };
    std::vector<size_t> order;
    while (order.size() < tags_.size()) {
      container::util::checkModelImportCancellation(stop_);
      while (!readyBoundaries.empty()) {
        const size_t boundary = readyBoundaries.back();
        readyBoundaries.pop_back();
        visit(boundary);
      }
      if (readyTags.empty()) {
        std::string context;
        for (const auto &row : rows_)
          if (std::ranges::any_of(row.runs, [&](const Run &run) {
                return incoming[run.tag] != 0;
              }))
            context += " #" + std::to_string(row.profileId);
        throw std::runtime_error(
            "Crossing or conflicting section tag order in profiles" + context);
      }
      if (readyTags.size() != 1) {
        const auto a = *readyTags.begin(), b = *std::next(readyTags.begin());
        throw std::runtime_error("Ambiguous section tag order for '" +
                                 *tags_[a].name + "' and '" + *tags_[b].name +
                                 "' between " + profile(rows_.front()) +
                                 " and " + profile(rows_.back()));
      }
      const auto tag = *readyTags.begin();
      readyTags.erase(tag);
      order.push_back(tag);
      visit(tag);
    }
    return order;
  }

  void validateReordering(std::span<const size_t> order) const {
    std::vector<size_t> rank(order.size());
    for (size_t k = 0; k < order.size(); ++k)
      rank[order[k]] = k;
    for (const auto &row : rows_)
      for (size_t i = 1; i < row.runs.size(); ++i) {
        const auto &a = row.runs[i - 1];
        const auto &b = row.runs[i];
        if (rank[a.tag] > rank[b.tag] &&
            row.clusters[a.first] != row.clusters[b.last])
          reject(row, "reorders noncoincident section tag runs");
      }
  }

  std::vector<std::vector<Anchor>> completeMissingRuns() {
    std::vector<std::vector<Anchor>> anchors(rows_.size(),
                                             std::vector<Anchor>(tags_.size()));
    for (size_t tag = 0; tag < tags_.size(); ++tag) {
      container::util::checkModelImportCancellation(stop_);
      if (tags_[tag].present == rows_.size())
        continue;
      for (size_t first = 0; first < rows_.size();) {
        if (rows_[first].runByTag[tag] != absent) {
          ++first;
          continue;
        }
        size_t last = first + 1;
        while (last < rows_.size() && rows_[last].runByTag[tag] == absent)
          ++last;
        const auto anchor = missingAnchor(tag, first, last);
        for (size_t i = first; i < last; ++i)
          anchors[i][tag] = anchor;
        first = last;
      }
    }
    return anchors;
  }

  Anchor missingAnchor(size_t tag, size_t first, size_t last) {
    std::optional<Anchor> selected;
    for (const size_t neighbor : {first == 0 ? absent : first - 1, last}) {
      if (neighbor >= rows_.size())
        continue;
      const auto &row = rows_[neighbor];
      const auto &run = row.runs[row.runByTag[tag]];
      const std::string note = "missing section tag '" + *tags_[tag].name +
                               "' at " + profile(rows_[first]);
      const size_t cluster = row.clusters[run.first];
      if (row.clusters[run.last] != cluster)
        reject(row,
               note +
                   " begins or ends with nonzero width and no coincident tip");
      const auto tipRange = std::ranges::equal_range(row.clusters, cluster);
      const auto tipStart = size_t(tipRange.begin() - row.clusters.begin());
      const auto tipEnd = size_t(tipRange.end() - row.clusters.begin() - 1);
      std::optional<Anchor> anchor;
      const auto candidates =
          std::ranges::lower_bound(row.runs, tipStart, {}, &Run::last);
      for (auto it = candidates; it != row.runs.end() && it->first <= tipEnd;
           ++it) {
        const auto &candidate = *it;
        if (candidate.tag == tag)
          continue;
        bool retained = true;
        for (size_t i = first; i < last && retained; ++i) {
          if (++anchorChecks_ > maxChecks)
            reject(row, "section tag anchors exceed their comparison budget");
          retained = rows_[i].runByTag[candidate.tag] != absent;
        }
        if (!retained)
          continue;
        if (anchor)
          reject(row, note + " has more than one retained coincident anchor");
        // A separate tag can join a repeated run only at the neighboring
        // boundary occurrence. A collapsed run may have several equal
        // points at this tip but its outer occurrence is still well defined.
        const double fraction =
            candidate.last < run.first &&
                    tags_[candidate.tag].fractions.size() != 1
                ? 1.
                : 0.;
        anchor = Anchor{candidate.tag, fraction};
      }
      if (!anchor)
        reject(row,
               note + " requires an authored coincident tip on a retained tag");
      if (selected && *selected != *anchor)
        reject(row, note +
                        " joins conflicting retained anchor occurrences for '" +
                        *tags_[selected->tag].name + "' and '" +
                        *tags_[anchor->tag].name + "'");
      selected = anchor;
    }
    if (!selected)
      reject(rows_[first], "missing section tag has no incident authored tip");
    return *selected;
  }

  IfcSectionTagLayout
  assemble(std::span<const size_t> order,
           const std::vector<std::vector<Anchor>> &anchors) const {
    IfcSectionTagLayout result;
    size_t count = 0;
    for (size_t tag : order)
      count += tags_[tag].fractions.size();
    if (count > maxColumns || count > maxPoints / rows_.size())
      throw std::runtime_error(
          "Aligned section tags exceed their point budget");
    size_t column = 0;
    std::vector<size_t> columnByTag(tags_.size());
    for (size_t id : order) {
      const auto &tag = tags_[id];
      columnByTag[id] = column;
      result.taggedColumns.emplace(
          *tag.name,
          std::pair(double(column) / (count - 1), tag.fractions.size() == 1));
      if (tag.present != rows_.size())
        result.intermittentTags.push_back(*tag.name);
      column += tag.fractions.size();
    }
    std::ranges::sort(result.intermittentTags);
    for (size_t i = 0; i < rows_.size(); ++i) {
      container::util::checkModelImportCancellation(stop_);
      auto &points = result.rows.emplace_back();
      auto &retained = result.retainedColumns.emplace_back();
      points.reserve(count);
      retained.reserve(count);
      const auto &row = rows_[i];
      std::optional<glm::dvec3> previous;
      size_t priorOccurrence = 0;
      for (size_t id : order) {
        const auto anchor = anchors[i][id];
        const bool missing = anchor.tag != absent;
        const auto &run = row.runs[row.runByTag[missing ? anchor.tag : id]];
        const size_t first =
            missing && anchor.fraction == 1 ? run.last : run.first;
        const size_t last = missing ? first : run.last;
        const auto retainedColumn =
            missing
                ? std::optional(columnByTag[anchor.tag] +
                                (anchor.fraction == 1
                                     ? tags_[anchor.tag].fractions.size() - 1
                                     : 0))
                : std::nullopt;
        if (first < priorOccurrence &&
            row.clusters[first] != row.clusters[priorOccurrence])
          reject(row, "aligned section tag '" + *tags_[id].name +
                          "' would cross an authored segment");
        priorOccurrence = std::max(priorOccurrence, last);
        for (double fraction : tags_[id].fractions) {
          retained.push_back(retainedColumn);
          const auto point =
              missing ? row.points[first] : atFraction(row, run, fraction);
          if (previous && coincident(*previous, point)) {
            points.push_back(*previous);
            result.coincidentEdges = true;
          } else {
            points.push_back(point);
          }
          previous = points.back();
        }
      }
    }
    return result;
  }

  double tolerance_;
  std::stop_token stop_;
  size_t sourcePoints_ = 0;
  size_t anchorChecks_ = 0;
  std::map<std::string, size_t> tagsByName_;
  std::vector<Tag> tags_;
  std::vector<Row> rows_;
};

} // namespace

IfcSectionTagLayout
alignIfcSectionTagRows(std::span<const IfcSectionTagRow> sections,
                       double sourceTolerance, std::stop_token stop) {
  return Alignment(sourceTolerance, stop).run(sections);
}

ParametricCurve alignIfcIndexedSectionCurve(ParametricCurve source,
                                           std::vector<double> parameters,
                                           uint32_t profileId) {
  const auto fail = [profileId](const char *reason) {
    throw std::runtime_error("Sectioned profile #" + std::to_string(profileId) +
                             ": " + reason);
  };
  if (!source.point || !source.domain || source.dimension != 2 ||
      source.period != 0 || parameters.size() < 2 ||
      parameters.size() > maxColumns || !std::isfinite(source.domain->first) ||
      !std::isfinite(source.domain->second - source.domain->first) ||
      parameters.front() != source.domain->first ||
      parameters.back() != source.domain->second)
    fail("indexed tag parameters must span a bounded 2D source curve");

  struct Slot {
    double native, aligned;
    glm::dvec3 point;
  };
  struct ParameterMap {
    ParametricCurve source;
    std::vector<Slot> slots;

    size_t interval(double v) const {
      const auto upper = std::ranges::lower_bound(slots, v, {}, &Slot::aligned);
      return upper == slots.begin() ? 0 : size_t(upper - slots.begin() - 1);
    }

    double nativeParameter(size_t i, double v) const {
      const auto &a = slots[i], &b = slots[i + 1];
      const double fraction = (v - a.aligned) / (b.aligned - a.aligned);
      return std::lerp(a.native, b.native, fraction);
    }

    std::optional<glm::dvec3> point(size_t i, double v) const {
      if (v == slots[i].aligned)
        return slots[i].point;
      if (v == slots[i + 1].aligned)
        return slots[i + 1].point;
      return source.point(nativeParameter(i, v));
    }
  };
  auto map = std::make_shared<ParameterMap>();
  map->source = std::move(source);
  map->slots.reserve(parameters.size());
  ParametricCurve curve;
  curve.domain = {{0., 1.}};
  curve.dimension = 2;
  curve.breaks.reserve(parameters.size() - 2);
  for (size_t i = 0; i < parameters.size(); ++i) {
    const double native = parameters[i];
    const double aligned = double(i) / double(parameters.size() - 1);
    if (!std::isfinite(native) || (i && !(native > parameters[i - 1])))
      fail("indexed tag parameters must be finite and strictly increasing");
    const auto point = map->source.point(native);
    if (!point || !finite(*point))
      fail("an indexed tag point is undefined on its source curve");
    map->slots.push_back({native, aligned, *point});
    if (i && i + 1 < parameters.size())
      curve.breaks.push_back(aligned);
  }
  const auto valid = [](double v) { return std::isfinite(v) && v >= 0 && v <= 1; };
  curve.point = [map, valid](double v) -> std::optional<glm::dvec3> {
    return valid(v) ? map->point(map->interval(v), v) : std::nullopt;
  };
  if (map->source.derivative)
    curve.derivative = [map, valid](double v) -> std::optional<glm::dvec3> {
      if (!valid(v))
        return std::nullopt;
      const size_t i = map->interval(v);
      const auto &a = map->slots[i], &b = map->slots[i + 1];
      const auto d = curveDerivative(map->source, map->nativeParameter(i, v));
      const double scale = (b.native - a.native) / (b.aligned - a.aligned);
      if (!d || !std::isfinite(scale))
        return std::nullopt;
      const auto derivative = *d * scale;
      return finite(derivative) ? std::optional(derivative) : std::nullopt;
    };
  curve.sampler = [map, valid](double a, double b, double error,
                              size_t limit) -> std::vector<CurveSample> {
    if (!valid(a) || !valid(b) || a == b || limit < 2)
      return {};
    const bool reversed = b < a;
    if (reversed)
      std::swap(a, b);
    std::vector<CurveSample> result;
    for (size_t i = map->interval(a);
         i + 1 < map->slots.size() && map->slots[i].aligned < b; ++i) {
      const auto &left = map->slots[i], &right = map->slots[i + 1];
      const double first = std::max(a, left.aligned),
                   last = std::min(b, right.aligned);
      if (!(last > first))
        continue;
      const size_t remaining = limit - result.size() + !result.empty();
      const double lo = map->nativeParameter(i, first);
      const double hi = map->nativeParameter(i, last);
      const auto part = sampleCurve(map->source, lo, hi, error, remaining);
      if (part.size() < 2 || part.size() > remaining)
        return {};
      // Re-pin native sampler endpoints to the exact requested nodes. A
      // source sampler may have derived them by arithmetic on its subdomain.
      const double epsilon =
          roundoff * std::max({std::abs(lo), std::abs(hi), hi - lo});
      if (!(std::abs(part.front().parameter - lo) <= epsilon) ||
          !(std::abs(part.back().parameter - hi) <= epsilon))
        return {};
      const auto start = map->point(i, first), end = map->point(i, last);
      if (!start || !end || !finite(*start) || !finite(*end))
        return {};
      if (result.empty())
        result.push_back({first, *start});
      for (size_t j = 1; j + 1 < part.size(); ++j) {
        const auto &sample = part[j];
        const double fraction =
            (sample.parameter - left.native) / (right.native - left.native);
        const double v = std::lerp(left.aligned, right.aligned, fraction);
        if (!(v > result.back().parameter && v < last) || !finite(sample.point))
          return {};
        result.push_back({v, sample.point});
      }
      result.push_back({last, *end});
    }
    if (reversed)
      std::ranges::reverse(result);
    return result;
  };
  return curve;
}

} // namespace container::geometry::ifc::detail
