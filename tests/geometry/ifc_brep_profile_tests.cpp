#include "../../src/geometry/IfcBrepFaceGeometry.h"
#include "Container/geometry/IfcTessellatedLoader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <gtest/gtest.h>
#include <iomanip>
#include <map>
#include <numbers>
#include <sstream>

namespace {
using container::geometry::ImportCompleteness;
using container::geometry::ifc::LoadFromStep;
using container::geometry::ifc::Model;
constexpr double pi = std::numbers::pi;

std::string number(double value) {
  std::ostringstream stream;
  stream << std::setprecision(17) << value;
  return stream.str();
}
std::string fixture(const std::string &definitions, std::string item = "#10") {
  return "ISO-10303-21; DATA; #1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);" +
         definitions + "#900001=IFCSHAPEREPRESENTATION($,'Body','Brep',(" +
         item +
         ")); #900002=IFCPRODUCTDEFINITIONSHAPE($,$,(#900001));"
         "#900003=IFCBUILDINGELEMENTPROXY('brep-profile',$,'Test',$,$,$,#"
         "900002,$,$);"
         "ENDSEC; END-ISO-10303-21;";
}
void exportCurvedFixture(const char *name, const std::string &source) {
  if (const auto *root = std::getenv("CONTAINER_IFC_CURVED_FIXTURE_ROOT")) {
    std::filesystem::create_directories(root);
    std::ofstream stream(std::filesystem::path(root) /
                         (std::string(name) + ".ifc"));
    stream << source;
    ASSERT_TRUE(stream.good());
  }
}
void complete(const Model &model) {
  for (const auto &diagnostic : model.importReport.diagnostics)
    ADD_FAILURE() << diagnostic.entityId << ": " << diagnostic.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  ASSERT_FALSE(model.elements.empty());
  EXPECT_EQ(model.importReport.importedProductCount, 1u);
  for (const auto &element : model.elements)
    EXPECT_EQ(element.guid, "brep-profile");
}
double volume(const Model &model) {
  double result = 0;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    const glm::dvec3 a = model.vertices.at(model.indices.at(i)).position;
    const glm::dvec3 b = model.vertices.at(model.indices.at(i + 1)).position;
    const glm::dvec3 c = model.vertices.at(model.indices.at(i + 2)).position;
    result += glm::dot(a, glm::cross(b, c)) / 6;
  }
  return result;
}
void watertight(const Model &model) {
  std::map<std::array<float, 3>, size_t> points;
  std::map<std::pair<size_t, size_t>, std::pair<unsigned, int>> edges;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    size_t triangle[3];
    for (size_t k = 0; k < 3; ++k) {
      const auto p = model.vertices.at(model.indices.at(i + k)).position;
      triangle[k] =
          points.try_emplace({p.x, p.y, p.z}, points.size()).first->second;
    }
    for (size_t k = 0; k < 3; ++k) {
      const auto a = triangle[k], b = triangle[(k + 1) % 3];
      ASSERT_NE(a, b);
      auto &edge = edges[{std::min(a, b), std::max(a, b)}];
      ++edge.first;
      edge.second += a < b ? 1 : -1;
    }
  }
  for (const auto &[_, edge] : edges) {
    EXPECT_EQ(edge.first, 2u);
    EXPECT_EQ(edge.second, 0);
  }
}
std::string degrees() {
  return "#70=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.);"
         "#71=IFCMEASUREWITHUNIT(IFCPLANEANGLEMEASURE(0.017453292519943295),#"
         "70);"
         "#72=IFCCONVERSIONBASEDUNIT($,.PLANEANGLEUNIT.,'Degree',#71);"
         "#73=IFCUNITASSIGNMENT((#1,#72));"
         "#74=IFCPROJECT('project',$,$,$,$,$,$,(),#73);";
}

TEST(IfcStructuralProfiles, SlopesHonorThicknessReferencesAndAngleUnits) {
  for (double taper : {-.1, .1}) {
    const double angle = std::atan(taper);
    // The L profile's missing quadrant splits into two triangles at the
    // intersection of x + y*taper = -2 and y + x*taper = -3.
    const double jx = (-2 + 3 * taper) / (1 - taper * taper);
    const double jy = (-3 + 2 * taper) / (1 - taper * taper);
    const double lArea =
        48 - .5 * ((3 - jx) * (7 + 3 * taper) + (4 - jy) * (5 + 4 * taper));
    for (bool degreeUnits : {false, true}) {
      const auto slope = number(angle * (degreeUnits ? 180 / pi : 1));
      for (const auto &[profile, area] :
           std::array<std::pair<std::string, double>, 3>{
               {{"IFCLSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,$,$," + slope + ")",
                 lArea},
                {"IFCUSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,1.,$,$," + slope +
                     ")",
                 18 - 5 * taper},
                {"IFCISHAPEPROFILEDEF(.AREA.,$,$,6.,8.,1.,1.,$,$," + slope +
                     ")",
                 18 + 12.5 * taper}}}) {
        const auto model = LoadFromStep(
            fixture("#5=" + profile + "; #10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);" +
                    (degreeUnits ? degrees() : "")));
        complete(model);
        ASSERT_FALSE(model.indices.empty());
        EXPECT_NEAR(volume(model), area * 3, 1e-5) << profile;
        watertight(model);
      }
    }
  }
}

TEST(IfcStructuralProfiles,
     SlopedFilletsAndRotatedPlacementRetainAnalyticVolume) {
  const double alpha = .1, taper = std::tan(alpha);
  const auto segmentArea = [](double turn) {
    return std::tan(turn / 2) - turn / 2;
  };
  const double filletArea = .25 * segmentArea(pi / 2 - alpha);
  const double tipArea = .04 * segmentArea(pi / 2 - alpha);
  const double jx = (-2 + 3 * taper) / (1 - taper * taper);
  const double jy = (-3 + 2 * taper) / (1 - taper * taper);
  const double lArea =
      48 - .5 * ((3 - jx) * (7 + 3 * taper) + (4 - jy) * (5 + 4 * taper));
  for (const auto &[profile, area] :
       std::array<std::pair<std::string, double>, 3>{
           {{"IFCLSHAPEPROFILEDEF(.AREA.,$,#4,8.,6.,1.,0.5,0.2,0.1)",
             lArea + .25 * segmentArea(pi / 2 - 2 * alpha) - 2 * tipArea},
            {"IFCUSHAPEPROFILEDEF(.AREA.,$,#4,8.,6.,1.,1.,0.5,0.2,0.1)",
             18 - 5 * taper + 2 * (filletArea - tipArea)},
            {"IFCISHAPEPROFILEDEF(.AREA.,$,#4,6.,8.,1.,1.,0.5,0.2,0.1)",
             18 + 12.5 * taper + 4 * (filletArea - tipArea)}}}) {
    const auto model = LoadFromStep(
        fixture("#2=IFCCARTESIANPOINT((9.,-7.)); #3=IFCDIRECTION((0.,1.));"
                "#4=IFCAXIS2PLACEMENT2D(#2,#3); #5=" +
                profile + "; #10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);"));
    complete(model);
    // Circular fillets use a 1 mm chord budget; allow the resulting polygonal
    // volume error while checking the independently integrated arc areas.
    EXPECT_NEAR(volume(model), area * 3, area * 3 * .0001);
    watertight(model);
    for (const auto &vertex : model.vertices) {
      EXPECT_GE(vertex.position.x, 5.f);
      EXPECT_LE(vertex.position.x, 13.f);
      EXPECT_GE(vertex.position.y, -10.f);
      EXPECT_LE(vertex.position.y, -4.f);
    }
  }
}

TEST(IfcStructuralProfiles, CollapsedAndAmbiguousSlopesRejectAtomically) {
  for (const auto &profile :
       {"IFCLSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,$,$,0.7853981633974483)",
        "IFCLSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,$,$,0.6)",
        "IFCUSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,1.,$,$,0.6)",
        "IFCISHAPEPROFILEDEF(.AREA.,$,$,6.,8.,1.,1.,$,$,1.2)",
        "IFCISHAPEPROFILEDEF(.AREA.,$,$,6.,8.,1.,1.,$,$,-0.6)",
        "IFCISHAPEPROFILEDEF(.AREA.,$,$,6.,8.,1.,1.,$,$,1.5707963267948966)"}) {
    const auto model =
        LoadFromStep(fixture("#5=" + std::string(profile) +
                             "; #10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.meshRanges.empty());
  }
  const auto invalidUnits = LoadFromStep(
      fixture("#5=IFCUSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,1.,$,$,0.1);"
              "#10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);"
              "#73=IFCUNITASSIGNMENT((#72)); "
              "#72=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.METRE.);"
              "#74=IFCPROJECT('project',$,$,$,$,$,$,(),#73);"));
  EXPECT_EQ(invalidUnits.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(invalidUnits.vertices.empty());
}

struct StepBuilder {
  std::string definitions;
  uint32_t next = 100;
  std::string add(const std::string &value) {
    const auto ref = "#" + std::to_string(next++);
    definitions += ref + "=" + value + ";";
    return ref;
  }
  std::string point(glm::dvec3 p) {
    return add("IFCCARTESIANPOINT((" + number(p.x) + "," + number(p.y) + "," +
               number(p.z) + "))");
  }
  std::string refs(const std::vector<std::string> &values) {
    std::string result;
    for (const auto &value : values) {
      if (!result.empty())
        result += ",";
      result += value;
    }
    return result;
  }
  // Generate the same geometry with independent faceted and edge topology.
  std::string prism(std::vector<std::vector<glm::dvec2>> rings,
                    glm::dvec3 origin, double height, bool advanced,
                    bool inward = false, unsigned curveFamily = 0) {
    std::vector<glm::dvec3> positions;
    std::vector<std::vector<unsigned>> lower, upper;
    std::vector<std::string> points, vertices;
    for (const auto &ring : rings) {
      std::vector<unsigned> bottom, top;
      for (auto p : ring) {
        bottom.push_back(static_cast<unsigned>(positions.size()));
        positions.push_back(origin + glm::dvec3(p, 0));
        top.push_back(static_cast<unsigned>(positions.size()));
        positions.push_back(origin + glm::dvec3(p, height));
      }
      lower.push_back(bottom);
      upper.push_back(top);
    }
    for (auto p : positions) {
      points.push_back(point(p));
      vertices.push_back(advanced ? add("IFCVERTEXPOINT(" + points.back() + ")")
                                  : "");
    }
    std::map<std::pair<unsigned, unsigned>, std::string> edges;
    std::vector<std::string> faces;
    auto face = [&](std::vector<std::vector<unsigned>> loops) {
      if (inward)
        for (auto &loop : loops)
          std::ranges::reverse(loop);
      std::vector<std::string> bounds;
      for (size_t ring = 0; ring < loops.size(); ++ring) {
        std::vector<std::string> entries;
        for (size_t j = 0; j < loops[ring].size(); ++j) {
          const auto a = loops[ring][j],
                     b = loops[ring][(j + 1) % loops[ring].size()];
          if (!advanced) {
            entries.push_back(points[a]);
            continue;
          }
          const auto key = std::minmax(a, b);
          if (!edges.contains(key)) {
            const auto start = key.first, end = key.second;
            const bool sameSense = edges.size() % 2 == 0;
            const auto x = sameSense ? start : end, y = sameSense ? end : start;
            std::string curve;
            if (curveFamily == 0) {
              const auto delta = positions[y] - positions[x];
              const auto direction =
                  add("IFCDIRECTION((" + number(delta.x) + "," +
                      number(delta.y) + "," + number(delta.z) + "))");
              curve = add("IFCLINE(" + points[x] + "," +
                          add("IFCVECTOR(" + direction + "," +
                              number(glm::length(delta)) + ")") +
                          ")");
            } else {
              const auto mid = point((positions[x] + positions[y]) / 2.);
              curve = curveFamily == 1
                          ? add("IFCPOLYLINE((" + points[x] + "," + mid + "," +
                                points[y] + "))")
                          : add("IFCBSPLINECURVEWITHKNOTS(1,(" + points[x] +
                                "," + mid + "," + points[y] +
                                "),.UNSPECIFIED.,.F.,.F.,(2,1,2),(0.,0.5,1.),."
                                "UNSPECIFIED.)");
            }
            edges[key] =
                add("IFCEDGECURVE(" + vertices[start] + "," + vertices[end] +
                    "," + curve + (sameSense ? ",.T.)" : ",.F.)"));
          }
          entries.push_back(add("IFCORIENTEDEDGE(*,*," + edges[key] +
                                ((a < b) != (ring == 0) ? ",.T.)" : ",.F.)")));
        }
        auto loop = add((advanced ? "IFCEDGELOOP((" : "IFCPOLYLOOP((") +
                        refs(entries) + "))");
        // Reverse an authored loop and recover it with boundary Orientation.
        if (ring == 0) {
          std::ranges::reverse(entries);
          loop = add((advanced ? "IFCEDGELOOP((" : "IFCPOLYLOOP((") +
                     refs(entries) + "))");
        }
        bounds.push_back(
            add((ring == 0 ? "IFCFACEOUTERBOUND(" : "IFCFACEBOUND(") + loop +
                (ring == 0 ? ",.F.)" : ",.T.)")));
      }
      if (!advanced) {
        faces.push_back(add("IFCFACE((" + refs(bounds) + "))"));
        return;
      }
      auto normal = glm::normalize(
          glm::cross(positions[loops[0][1]] - positions[loops[0][0]],
                     positions[loops[0][2]] - positions[loops[0][0]]));
      const bool sameSense = faces.size() % 2 == 0;
      if (!sameSense)
        normal = -normal;
      const auto direction =
          add("IFCDIRECTION((" + number(normal.x) + "," + number(normal.y) +
              "," + number(normal.z) + "))");
      const auto plane = add("IFCPLANE(" +
                             add("IFCAXIS2PLACEMENT3D(" + points[loops[0][0]] +
                                 "," + direction + ",$)") +
                             ")");
      faces.push_back(add("IFCADVANCEDFACE((" + refs(bounds) + ")," + plane +
                          (sameSense ? ",.T.)" : ",.F.)")));
    };
    auto bottom = lower;
    for (auto &ring : bottom)
      std::ranges::reverse(ring);
    face(bottom);
    face(upper);
    for (size_t ring = 0; ring < lower.size(); ++ring)
      for (size_t j = 0; j < lower[ring].size(); ++j) {
        const auto k = (j + 1) % lower[ring].size();
        face(
            {{lower[ring][j], lower[ring][k], upper[ring][k], upper[ring][j]}});
      }
    return add("IFCCLOSEDSHELL((" + refs(faces) + "))");
  }
};
struct CurvedBuilder : StepBuilder {
  std::map<std::string, std::string> vertices;
  std::vector<std::string> faces;
  std::string direction(glm::dvec3 p) {
    return add("IFCDIRECTION((" + number(p.x) + "," + number(p.y) + "," +
               number(p.z) + "))");
  }
  std::string placement(glm::dvec3 origin, glm::dvec3 axis = {0, 0, 1},
                        glm::dvec3 x = {1, 0, 0}) {
    return add("IFCAXIS2PLACEMENT3D(" + point(origin) + "," + direction(axis) +
               "," + direction(x) + ")");
  }
  std::string edge(const std::string &a, const std::string &b,
                   const std::string &curve) {
    for (const auto &p : {a, b})
      if (!vertices.contains(p))
        vertices[p] = add("IFCVERTEXPOINT(" + p + ")");
    return add("IFCEDGECURVE(" + vertices[a] + "," + vertices[b] + "," + curve +
               ",.T.)");
  }
  std::string line(const std::string &a, const std::string &b) {
    return edge(a, b, add("IFCPOLYLINE((" + a + "," + b + "))"));
  }
  using Use = std::pair<std::string, bool>;
  std::string bound(std::vector<Use> uses, bool outer = true,
                    bool orientation = true) {
    if (!orientation) {
      std::ranges::reverse(uses);
      for (auto &use : uses)
        use.second = !use.second;
    }
    std::vector<std::string> oriented;
    for (const auto &[ref, sense] : uses)
      oriented.push_back(
          add("IFCORIENTEDEDGE(*,*," + ref + (sense ? ",.T.)" : ",.F.)")));
    return add(std::string(outer ? "IFCFACEOUTERBOUND(" : "IFCFACEBOUND(") +
               add("IFCEDGELOOP((" + refs(oriented) + "))") +
               (orientation ? ",.T.)" : ",.F.)"));
  }
  void face(const std::vector<std::string> &bounds, const std::string &surface,
            bool sense = true) {
    faces.push_back(add("IFCADVANCEDFACE((" + refs(bounds) + ")," + surface +
                        (sense ? ",.T.)" : ",.F.)")));
  }
  void plane(std::vector<Use> uses, glm::dvec3 origin, glm::dvec3 normal) {
    const auto x =
        std::abs(normal.x) < .8 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    face({bound(std::move(uses))},
         add("IFCPLANE(" + placement(origin, normal, x) + ")"));
  }
  struct Circle {
    std::string point, edge;
  };
  Circle circle(double radius, double z, double start = 0) {
    const auto p =
        point({radius * std::cos(start), radius * std::sin(start), z});
    return {p, edge(p, p,
                    add("IFCCIRCLE(" + placement({0, 0, z}) + "," +
                        number(radius) + ")"))};
  }
  std::string vertexBound(glm::dvec3 location, bool outer = false,
                          bool orientation = true) {
    return add(std::string(outer ? "IFCFACEOUTERBOUND(" : "IFCFACEBOUND(") +
               add("IFCVERTEXLOOP(" +
                   add("IFCVERTEXPOINT(" + point(location) + ")") + ")") +
               (orientation ? ",.T.)" : ",.F.)"));
  }
  std::string finish() {
    return add("IFCADVANCEDBREP(" +
               add("IFCCLOSEDSHELL((" + refs(faces) + "))") + ")");
  }
  std::string band(const std::string &type, double radius, double z1, double z2,
                   double ringRadius, bool reversedBounds = false,
                   double minor = 0, double topStart = 0,
                   double bottomStart = 0, double topRadius = -1) {
    const auto bottom = circle(ringRadius, z1, bottomStart),
               top =
                   circle(topRadius > 0 ? topRadius : ringRadius, z2, topStart);
    const auto surface =
        add(type + "(" + placement({0, 0, 0}) + "," + number(radius) +
            (minor ? "," + number(minor) : "") + ")");
    auto low = bound({{bottom.edge, true}}, false, !reversedBounds);
    auto high = bound({{top.edge, false}}, false, !reversedBounds);
    face(reversedBounds ? std::vector{high, low} : std::vector{low, high},
         surface);
    plane({{bottom.edge, false}}, {0, 0, z1}, {0, 0, -1});
    plane({{top.edge, true}}, {0, 0, z2}, {0, 0, 1});
    return finish();
  }
  std::string splineSector(double radius, double height, bool rational) {
    const auto a = point({radius, 0, 0}), b = point({0, radius, 0}),
               c = point({radius, 0, height}), d = point({0, radius, height}),
               o = point({0, 0, 0}), p = point({0, 0, height});
    const auto m = point({radius, radius, 0}),
               n = point({radius, radius, height});
    const std::string weights =
        rational ? ",(1.,0.7071067811865475244,1.)" : "";
    const auto curve = [&](const std::string &x, const std::string &y,
                           const std::string &z) {
      return add(std::string(rational ? "IFCRATIONALBSPLINECURVEWITHKNOTS"
                                      : "IFCBSPLINECURVEWITHKNOTS") +
                 "(2,(" + x + "," + y + "," + z +
                 "),.UNSPECIFIED.,.F.,.F.,(3,3),(2.,5.),.UNSPECIFIED." +
                 weights + ")");
    };
    const auto bottom = edge(a, b, curve(a, m, b)),
               top = edge(c, d, curve(c, n, d)), start = line(a, c),
               end = line(b, d), ac = line(o, a), bc = line(o, b),
               cc = line(p, c), dc = line(p, d), axis = line(o, p);
    const std::string grid =
        "((" + a + "," + c + "),(" + m + "," + n + "),(" + b + "," + d + "))";
    const auto surface =
        add(std::string(rational ? "IFCRATIONALBSPLINESURFACEWITHKNOTS"
                                 : "IFCBSPLINESURFACEWITHKNOTS") +
            "(2,1," + grid +
            ",.UNSPECIFIED.,.F.,.F.,.F.,(3,3),(2,2),(2.,5.),(-1.,3.),."
            "UNSPECIFIED." +
            (rational ? ",((1.,1.),(0.7071067811865475244,0."
                        "7071067811865475244),(1.,1.))"
                      : "") +
            ")");
    face({bound({{bottom, true}, {end, true}, {top, false}, {start, false}})},
         surface);
    plane({{bottom, false}, {ac, false}, {bc, true}}, {0, 0, 0}, {0, 0, -1});
    plane({{cc, true}, {top, true}, {dc, false}}, {0, 0, height}, {0, 0, 1});
    plane({{ac, true}, {start, true}, {cc, false}, {axis, false}}, {0, 0, 0},
          {0, -1, 0});
    plane({{bc, false}, {axis, true}, {dc, true}, {end, false}}, {0, 0, 0},
          {-1, 0, 0});
    return finish();
  }
};

TEST(IfcCurvedBrep, FullCylindersPreserveSeamsBoundsAndPhysicalUnits) {
  for (bool reversed : {false, true})
    for (bool millimetres : {false, true}) {
      CurvedBuilder step;
      const double scale = millimetres ? 1000 : 1;
      const auto brep = step.band("IFCCYLINDRICALSURFACE", 2 * scale, 0,
                                  3 * scale, 2 * scale, reversed);
      auto source = fixture(step.definitions + degrees(), brep);
      if (millimetres)
        source.replace(source.find(".LENGTHUNIT.,$,.METRE."),
                       std::string(".LENGTHUNIT.,$,.METRE.").size(),
                       ".LENGTHUNIT.,.MILLI.,.METRE.");
      const auto model = LoadFromStep(source);
      complete(model);
      ASSERT_FALSE(model.indices.empty());
      watertight(model);
      EXPECT_NEAR(volume(model) / (scale * scale * scale), 12 * pi, .03);
      if (!reversed && !millimetres)
        exportCurvedFixture("cylinder", source);
      for (const auto &vertex : model.vertices)
        if (std::abs(vertex.normal.z) < .9f) {
          EXPECT_GT(vertex.position.x * vertex.normal.x +
                        vertex.position.y * vertex.normal.y,
                    1.99f * static_cast<float>(scale));
          EXPECT_NEAR(std::hypot(vertex.position.x, vertex.position.y),
                      2 * scale, .0011 * scale);
        }
    }
}

TEST(IfcCurvedBrep, SphericalBandsMatchIndependentSlabVolumeAndNormals) {
  CurvedBuilder step;
  const double radius = 2, z = .8, ring = std::sqrt(radius * radius - z * z);
  const auto brep = step.band("IFCSPHERICALSURFACE", radius, -z, z, ring);
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), pi * (radius * radius * 2 * z - 2 * z * z * z / 3),
              .025);
  float minimumNormalAgreement = 2;
  for (const auto &vertex : model.vertices)
    if (std::abs(vertex.normal.z) < .9f)
      minimumNormalAgreement = std::min(
          minimumNormalAgreement, glm::dot(vertex.position, vertex.normal));
  EXPECT_GT(minimumNormalAgreement, 1.995f);
  exportCurvedFixture("sphere", fixture(step.definitions, brep));
}

TEST(IfcCurvedBrep, OffsetPeriodicSeamsPreserveSharedCapsAndPhysicalUnits) {
  for (double start : {.17, 3.13, 6.271})
    for (bool reversed : {false, true})
      for (bool millimetres : {false, true}) {
        SCOPED_TRACE(::testing::Message() << start << ", reversed=" << reversed
                                          << ", millimetres=" << millimetres);
        CurvedBuilder step;
        const double scale = millimetres ? 1000 : 1;
        const auto brep =
            step.band("IFCCYLINDRICALSURFACE", 2 * scale, 0, 3 * scale,
                      2 * scale, reversed, 0, start, -.29);
        auto source = fixture(step.definitions + degrees(), brep);
        if (millimetres)
          source.replace(source.find(".LENGTHUNIT.,$,.METRE."),
                         std::string(".LENGTHUNIT.,$,.METRE.").size(),
                         ".LENGTHUNIT.,.MILLI.,.METRE.");
        const auto model = LoadFromStep(source);
        complete(model);
        ASSERT_FALSE(model.indices.empty());
        watertight(model);
        EXPECT_NEAR(volume(model) / (scale * scale * scale), 12 * pi, .03);
        for (const auto &vertex : model.vertices)
          if (std::abs(vertex.normal.z) < .9f) {
            EXPECT_GT(vertex.position.x * vertex.normal.x +
                          vertex.position.y * vertex.normal.y,
                      1.99f * static_cast<float>(scale));
            EXPECT_NEAR(std::hypot(vertex.position.x, vertex.position.y),
                        2 * scale, .0011 * scale);
          }
        if (start == .17 && !reversed && !millimetres)
          exportCurvedFixture("offset-cylinder", source);
      }
}

TEST(IfcCurvedBrep, OffsetSphericalBandsRetainUnequalBoundarySampling) {
  const double radius = 2, low = 0, high = 1.98;
  for (bool reversed : {false, true}) {
    CurvedBuilder step;
    const auto brep =
        step.band("IFCSPHERICALSURFACE", radius, low, high, radius, reversed, 0,
                  .173, -.31, std::sqrt(radius * radius - high * high));
    const auto source = fixture(step.definitions, brep);
    const auto model = LoadFromStep(source);
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model),
                pi * (radius * radius * (high - low) -
                      (high * high * high - low * low * low) / 3),
                .025);
    for (const auto &vertex : model.vertices)
      if (std::abs(vertex.normal.z) < .999f)
        EXPECT_GT(glm::dot(vertex.position, vertex.normal), 1.995f);
    if (!reversed)
      exportCurvedFixture("offset-sphere", source);
  }
}

TEST(IfcCurvedBrep, OffsetBandsSupportEitherPeriodicAxisAndSurfaceSense) {
  using container::geometry::ifc::detail::meshIfcCurvedFace;
  using container::geometry::ifc::detail::ParametricSurface;
  for (bool swapped : {false, true})
    for (bool sameSense : {false, true}) {
      ParametricSurface surface;
      const unsigned axis = swapped ? 1 : 0;
      surface.periods[axis] = 360;
      surface.point = [swapped](double u,
                                double v) -> std::optional<glm::dvec3> {
        const double angle = (swapped ? v : u) * pi / 180, z = swapped ? u : v;
        return glm::dvec3(2 * std::cos(angle), 2 * std::sin(angle), z);
      };
      surface.inverse = [swapped](glm::dvec3 p) -> std::optional<glm::dvec2> {
        const double angle = std::atan2(p.y, p.x) * 180 / pi;
        return swapped ? glm::dvec2(p.z, angle) : glm::dvec2(angle, p.z);
      };
      const double direction = (sameSense != swapped) ? 1 : -1;
      const auto ring = [&](unsigned count, double start, double z,
                            double sense) {
        std::vector<glm::vec3> points;
        for (unsigned i = 0; i < count; ++i) {
          const double angle = start + sense * 360 * i / count;
          points.emplace_back(
              *surface.point(swapped ? z : angle, swapped ? angle : z));
        }
        return points;
      };
      const auto bottom = ring(128, -13, 0, direction),
                 top = ring(160, 19.1, 3, -direction);
      std::string error;
      const auto mesh =
          meshIfcCurvedFace(surface, {bottom, top}, false, sameSense, 1, error);
      ASSERT_TRUE(mesh) << error;
      using Key = std::array<float, 3>;
      const auto key = [](glm::vec3 p) { return Key{p.x, p.y, p.z}; };
      std::map<std::pair<Key, Key>, std::pair<unsigned, int>> edges;
      for (const auto &triangle : *mesh)
        for (unsigned i = 0; i < 3; ++i) {
          const auto p = triangle.positions[i], n = triangle.normals[i];
          const double radialSense = swapped == sameSense ? -1 : 1;
          EXPECT_GT((p.x * n.x + p.y * n.y) * radialSense, 1.995);
          EXPECT_NEAR(std::hypot(p.x, p.y), 2., .0011);
          const auto a = key(p), b = key(triangle.positions[(i + 1) % 3]);
          ASSERT_NE(a, b);
          auto &use = edges[std::minmax(a, b)];
          ++use.first;
          use.second += a < b ? 1 : -1;
        }
      size_t boundaries = 0;
      for (const auto &[_, use] : edges) {
        EXPECT_LE(use.first, 2u);
        if (use.first == 1)
          ++boundaries;
        else
          EXPECT_EQ(use.second, 0);
      }
      EXPECT_EQ(boundaries, bottom.size() + top.size());
      for (const auto *loop : {&bottom, &top})
        for (size_t i = 0; i < loop->size(); ++i) {
          const auto a = key((*loop)[i]),
                     b = key((*loop)[(i + 1) % loop->size()]);
          const auto found = edges.find(std::minmax(a, b));
          ASSERT_NE(found, edges.end());
          EXPECT_EQ(found->second.first, 1u);
          EXPECT_EQ(found->second.second, a < b ? 1 : -1);
        }
      // A twice-wound loop is not a single periodic band, even if its paired
      // loop winds oppositely. It must be rejected before creating geometry.
      auto twiceBottom = bottom, twiceTop = top;
      twiceBottom.insert(twiceBottom.end(), bottom.begin(), bottom.end());
      twiceTop.insert(twiceTop.end(), top.begin(), top.end());
      EXPECT_FALSE(meshIfcCurvedFace(surface, {twiceBottom, twiceTop}, false,
                                     sameSense, 1, error));
      EXPECT_NE(error.find("multiple winding"), std::string::npos) << error;
    }
}

TEST(IfcCurvedBrep, SphericalVertexLoopsMeshClosedSpheresWithSmoothPoles) {
  for (int pole : {-1, 1})
    for (bool millimetres : {false, true}) {
      CurvedBuilder step;
      const double scale = millimetres ? 1000 : 1, radius = 2 * scale;
      const glm::dvec3 center(3 * scale, -4 * scale, 2 * scale), axis(0, 1, 0);
      const auto surface =
          step.add("IFCSPHERICALSURFACE(" + step.placement(center, axis) + "," +
                   number(radius) + ")");
      step.face({step.vertexBound(center + axis * (pole * radius), pole == 1,
                                  pole == 1)},
                surface);
      const auto brep = step.finish();
      auto source = fixture(step.definitions + degrees(), brep);
      if (millimetres)
        source.replace(source.find(".LENGTHUNIT.,$,.METRE."),
                       std::string(".LENGTHUNIT.,$,.METRE.").size(),
                       ".LENGTHUNIT.,.MILLI.,.METRE.");
      const auto model = LoadFromStep(source);
      complete(model);
      ASSERT_FALSE(model.indices.empty());
      watertight(model);
      EXPECT_NEAR(volume(model) / (scale * scale * scale), 32 * pi / 3, .06);
      bool foundPole = false;
      for (const auto &vertex : model.vertices) {
        const auto relative = glm::dvec3(vertex.position) - center;
        EXPECT_NEAR(glm::length(relative) / scale, 2., .0011);
        EXPECT_GT(glm::dot(relative / radius, glm::dvec3(vertex.normal)),
                  .99999);
        foundPole |=
            glm::length(relative - axis * (pole * radius)) < scale * 1e-6;
      }
      EXPECT_TRUE(foundPole);
      if (pole == 1 && !millimetres)
        exportCurvedFixture("pole-sphere", source);
    }
}

TEST(IfcCurvedBrep, SphericalPoleCapsRetainSharedRingAndAnalyticVolume) {
  for (int pole : {-1, 1})
    for (bool outer : {false, true})
      for (double latitude : {.8, -1.8}) {
        SCOPED_TRACE(::testing::Message()
                     << "pole=" << pole << ", outer=" << outer
                     << ", latitude=" << latitude);
        CurvedBuilder step;
        const double radius = 2, z = pole * latitude,
                     ringRadius = std::sqrt(radius * radius - z * z);
        const auto ring = step.circle(ringRadius, z, .193);
        const auto surface = step.add("IFCSPHERICALSURFACE(" +
                                      step.placement({0, 0, 0}) + ",2.)");
        step.face({step.vertexBound({0, 0, pole * radius}),
                   step.bound({{ring.edge, pole == 1}}, outer, false)},
                  surface);
        step.plane({{ring.edge, pole == -1}}, {0, 0, z},
                   {0, 0, static_cast<double>(-pole)});
        const auto brep = step.finish();
        const auto source = fixture(step.definitions, brep);
        const auto model = LoadFromStep(source);
        complete(model);
        ASSERT_FALSE(model.indices.empty());
        watertight(model);
        const double height = radius - latitude;
        EXPECT_NEAR(volume(model), pi * height * height * (radius - height / 3),
                    .06);
        for (const auto &vertex : model.vertices)
          if (std::hypot(vertex.normal.x, vertex.normal.y) > .001f)
            EXPECT_GT(glm::dot(vertex.position, vertex.normal), 1.995f);
        if (pole == 1 && outer && latitude == .8)
          exportCurvedFixture("pole-cap", source);
      }
}

TEST(IfcCurvedBrep, SphericalVertexLoopVoidsKeepInwardNormalsAndVolume) {
  CurvedBuilder step;
  std::vector<std::string> shells;
  for (double radius : {3., 1.}) {
    const auto surface =
        step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + "," +
                 number(radius) + ")");
    step.face({step.vertexBound({0, 0, radius})}, surface, radius == 3);
    shells.push_back(step.add("IFCCLOSEDSHELL((" + step.faces.back() + "))"));
  }
  const auto brep = step.add("IFCADVANCEDBREPWITHVOIDS(" + shells[0] + ",(" +
                             shells[1] + "))");
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 4 * pi * (27 - 1) / 3, .1);
  for (const auto &vertex : model.vertices) {
    const float radius = glm::length(vertex.position);
    EXPECT_NEAR(glm::dot(vertex.position / radius, vertex.normal),
                radius > 2 ? 1.f : -1.f, .00001f);
  }
}

TEST(IfcCurvedBrep, MalformedAndUnsupportedVertexLoopsRejectAtomically) {
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    CurvedBuilder step;
    const auto surface =
        step.add(std::string(invalid == 4 ? "IFCCYLINDRICALSURFACE("
                                          : "IFCSPHERICALSURFACE(") +
                 step.placement({0, 0, 0}) + ",2.)");
    auto bound = step.vertexBound(invalid == 0   ? glm::dvec3(2, 0, 0)
                                  : invalid == 1 ? glm::dvec3(0, 0, 1.9)
                                                 : glm::dvec3(0, 0, 2));
    std::vector bounds{bound};
    if (invalid == 2)
      bounds.push_back(step.vertexBound({0, 0, -2}));
    if (invalid == 3) {
      const auto at = step.definitions.find("=IFCVERTEXPOINT(");
      step.definitions.replace(at + 1, std::string("IFCVERTEXPOINT").size(),
                               "IFCVERTEX");
    }
    step.face(bounds, surface);
    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_NE(model.importReport.diagnostics.front().reason.find("loop"),
              std::string::npos)
        << model.importReport.diagnostics.front().reason;
  }
}

TEST(IfcCurvedBrep, ToroidalBandsRetainAnnularCapsAndInnerSurfaceSense) {
  for (bool offset : {false, true}) {
    SCOPED_TRACE(offset);
    CurvedBuilder step;
    const double major = 3, minor = 1, angle = .6, z = minor * std::sin(angle),
                 radius = major + minor * std::cos(angle);
    const auto bottom = step.circle(radius, -z, offset ? -.32 : 0),
               top = step.circle(radius, z, offset ? .19 : 0),
               innerBottom = step.circle(major, -z, offset ? .47 : 0),
               innerTop = step.circle(major, z, offset ? -.18 : 0);
    const auto torus = step.add("IFCTOROIDALSURFACE(" +
                                step.placement({0, 0, 0}) + ",3.,1.)"),
               cylinder = step.add("IFCCYLINDRICALSURFACE(" +
                                   step.placement({0, 0, 0}) + ",3.)");
    step.face({step.bound({{bottom.edge, true}}, false, !offset),
               step.bound({{top.edge, false}}, false, !offset)},
              torus);
    step.face({step.bound({{innerTop.edge, true}}, false),
               step.bound({{innerBottom.edge, false}}, false)},
              cylinder, false);
    step.face({step.bound({{top.edge, true}}),
               step.bound({{innerTop.edge, false}}, false)},
              step.add("IFCPLANE(" + step.placement({0, 0, z}) + ")"));
    step.face(
        {step.bound({{bottom.edge, false}}),
         step.bound({{innerBottom.edge, true}}, false)},
        step.add("IFCPLANE(" + step.placement({0, 0, -z}, {0, 0, -1}) + ")"));
    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    const double expected =
        pi * (2 * major * minor * minor *
                  (angle + std::sin(angle) * std::cos(angle)) +
              2 * minor * minor * minor *
                  (std::sin(angle) - std::pow(std::sin(angle), 3) / 3));
    EXPECT_NEAR(volume(model), expected, .045);
    exportCurvedFixture(offset ? "offset-torus" : "torus",
                        fixture(step.definitions, brep));
  }
}

TEST(IfcCurvedBrep, BilinearSplineBoxPreservesNonplanarInteriorAndFaceNormals) {
  CurvedBuilder step;
  const std::array<glm::dvec3, 8> locations{{{0, 0, 0},
                                             {2, 0, 0},
                                             {2, 2, 0},
                                             {0, 2, 0},
                                             {0, 0, 1},
                                             {2, 0, 1},
                                             {2, 2, 2},
                                             {0, 2, 1}}};
  std::vector<std::string> points;
  for (auto p : locations)
    points.push_back(step.point(p));
  std::map<std::pair<unsigned, unsigned>, std::string> edges;
  const auto makeFace = [&](std::vector<unsigned> loop, bool spline = false) {
    std::vector<CurvedBuilder::Use> uses;
    for (size_t i = 0; i < loop.size(); ++i) {
      const auto a = loop[i], b = loop[(i + 1) % loop.size()];
      const auto key = std::minmax(a, b);
      if (!edges.contains(key))
        edges[key] = step.line(points[key.first], points[key.second]);
      uses.push_back({edges[key], a < b});
    }
    if (!spline) {
      const auto n =
          glm::normalize(glm::cross(locations[loop[1]] - locations[loop[0]],
                                    locations[loop[2]] - locations[loop[0]]));
      step.plane(uses, locations[loop[0]], n);
    } else {
      const auto surface =
          step.add("IFCBSPLINESURFACEWITHKNOTS(1,1,((" + points[4] + "," +
                   points[7] + "),(" + points[5] + "," + points[6] +
                   ")),.UNSPECIFIED.,.F.,.F.,.F.,(2,2),(2,2),(-2.,3.),(1.,5.),."
                   "UNSPECIFIED.)");
      step.face({step.bound(uses)}, surface);
    }
  };
  makeFace({3, 2, 1, 0});
  makeFace({0, 1, 5, 4});
  makeFace({1, 2, 6, 5});
  makeFace({2, 3, 7, 6});
  makeFace({3, 0, 4, 7});
  makeFace({4, 5, 6, 7}, true);
  const auto brep = step.finish();
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 5., .003);
  float minimum = 1;
  for (const auto &vertex : model.vertices)
    if (vertex.normal.z > .7f) {
      const auto expected = glm::normalize(
          glm::vec3(-vertex.position.y * .25f, -vertex.position.x * .25f, 1));
      minimum = std::min(minimum, glm::dot(vertex.normal, expected));
    }
  EXPECT_GT(minimum, .99999f);
  exportCurvedFixture("bilinear", fixture(step.definitions, brep));
}

TEST(IfcCurvedBrep, SweptSurfaceAndExplicitSeamBoundsReuseSharedCurves) {
  for (unsigned family : {0u, 1u, 2u}) {
    const bool extruded = family == 1;
    CurvedBuilder step;
    const auto bottom = step.circle(2, 0), top = step.circle(2, 3);
    std::string surface;
    if (extruded) {
      const auto circle =
          step.add("IFCCIRCLE(" +
                   step.add("IFCAXIS2PLACEMENT2D(" +
                            step.add("IFCCARTESIANPOINT((0.,0.))") + ",$)") +
                   ",2.)");
      const auto profile =
          step.add("IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$," + circle + ")");
      surface = step.add("IFCSURFACEOFLINEAREXTRUSION(" + profile + ",$," +
                         step.direction({0, 0, 1}) + ",3.)");
    } else if (family == 2) {
      const auto a = step.add("IFCCARTESIANPOINT((2.,0.))"),
                 b = step.add("IFCCARTESIANPOINT((2.,3.))");
      const auto profile =
          step.add("IFCARBITRARYOPENPROFILEDEF(.CURVE.,$," +
                   step.add("IFCPOLYLINE((" + a + "," + b + "))") + ")");
      const auto axis = step.add("IFCAXIS1PLACEMENT(" + step.point({0, 0, 0}) +
                                 "," + step.direction({0, 1, 0}) + ")");
      surface =
          step.add("IFCSURFACEOFREVOLUTION(" + profile + "," +
                   step.placement({0, 0, 0}, {0, -1, 0}) + "," + axis + ")");
    } else
      surface = step.add("IFCCYLINDRICALSURFACE(" + step.placement({0, 0, 0}) +
                         ",2.)");
    const auto pcurve = [&](double u) {
      const auto a = step.add("IFCCARTESIANPOINT((" + number(u) + ",0.))"),
                 b = step.add("IFCCARTESIANPOINT((" + number(u) + "," +
                              (family ? "1." : "3.") + "))");
      return step.add("IFCPCURVE(" + surface + "," +
                      step.add("IFCPOLYLINE((" + a + "," + b + "))") + ")");
    };
    const auto seamCurve = step.add(
        "IFCSEAMCURVE(" +
        step.add("IFCPOLYLINE((" + bottom.point + "," + top.point + "))") +
        ",(" + pcurve(0) + "," + pcurve(2 * pi) + "),.CURVE3D.)");
    const auto seam = step.edge(bottom.point, top.point, seamCurve);
    step.face({step.bound({{bottom.edge, true},
                           {seam, true},
                           {top.edge, false},
                           {seam, false}})},
              surface);
    step.plane({{bottom.edge, false}}, {0, 0, 0}, {0, 0, -1});
    step.plane({{top.edge, true}}, {0, 0, 3}, {0, 0, 1});
    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model), 12 * pi, .03);
  }
}

TEST(IfcCurvedBrep,
     PolynomialAndRationalSplineFacesRetainKnotDomainsAndVolume) {
  for (bool rational : {false, true}) {
    CurvedBuilder step;
    const auto brep = step.splineSector(2, 3, rational);
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    // Polynomial quadratic: x=R(1-t^2), y=R(2t-t^2), area=5R^2/6.
    EXPECT_NEAR(volume(model), rational ? 3 * pi : 10., .015);
    EXPECT_GT(model.indices.size(), 100u);
    exportCurvedFixture(rational ? "rational" : "polynomial",
                        fixture(step.definitions, brep));
  }
}

TEST(IfcCurvedBrep, InvalidSurfacesAndOrientationRejectAtomically) {
  for (unsigned invalid = 0; invalid < 3; ++invalid) {
    CurvedBuilder step;
    const auto brep =
        step.band("IFCCYLINDRICALSURFACE", invalid == 0 ? 1.9 : 2., 0, 3, 2,
                  false, 0, .17);
    if (invalid == 1) {
      const auto p = step.definitions.find("=IFCADVANCEDFACE(");
      const auto flag = step.definitions.find(",.T.);", p);
      step.definitions.replace(flag + 2, 1, "F");
    } else if (invalid == 2) {
      const auto p = step.definitions.find("=IFCCYLINDRICALSURFACE(");
      const auto end = step.definitions.find(");", p);
      const auto comma = step.definitions.rfind(',', end);
      step.definitions.replace(comma + 1, end - comma - 1, "-2.");
    }
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed)
        << invalid;
    EXPECT_TRUE(model.vertices.empty()) << invalid;
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_NE(model.importReport.diagnostics.front().reason.find("Face #"),
              std::string::npos);
  }
}

TEST(IfcCurvedBrep, LargeExtrudedSurfaceAccountsForSharedFloatEdgeRounding) {
  CurvedBuilder step;
  const auto brep = step.band("IFCCYLINDRICALSURFACE", 100, 0, 3, 100);
  const auto at = step.definitions.find("=IFCCYLINDRICALSURFACE(");
  const auto start = step.definitions.rfind('#', at);
  const auto oldSurface = step.definitions.substr(start, at - start);
  const auto origin = step.add("IFCCARTESIANPOINT((0.,0.))");
  const auto circle =
      step.add("IFCCIRCLE(" +
               step.add("IFCAXIS2PLACEMENT2D(" + origin + ",$)") + ",100.)");
  const auto profile =
      step.add("IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$," + circle + ")");
  const auto surface = step.add("IFCSURFACEOFLINEAREXTRUSION(" + profile +
                                ",$," + step.direction({0, 0, 1}) + ",3.)");
  const auto faceReference = step.definitions.find(")," + oldSurface + ",.T.)");
  ASSERT_NE(faceReference, std::string::npos);
  step.definitions.replace(faceReference + 2, oldSurface.size(), surface);
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 30000 * pi, 5);
}

TEST(IfcCurvedBrep,
     CurvedFaceHolesAreConstrainedAndMalformedChartsAreRejected) {
  using container::geometry::ifc::detail::meshIfcCurvedFace;
  using container::geometry::ifc::detail::ParametricSurface;
  ParametricSurface s;
  s.point = [](double u, double v) -> std::optional<glm::dvec3> {
    return glm::dvec3(u, v, .15 * u * v);
  };
  s.uDomain = {{0, 2}};
  s.vDomain = {{0, 2}};
  s.inverse = [](glm::dvec3 p) -> std::optional<glm::dvec2> {
    return glm::dvec2(p.x, p.y);
  };
  const auto ring = [&](std::vector<glm::dvec2> uv) {
    std::vector<glm::vec3> points;
    for (auto p : uv)
      points.emplace_back(*s.point(p.x, p.y));
    return points;
  };
  const auto outer = ring({{0, 0}, {2, 0}, {2, 2}, {0, 2}}),
             inner = ring({{.75, .75}, {.75, 1.25}, {1.25, 1.25}, {1.25, .75}});
  std::string error;
  const auto mesh = meshIfcCurvedFace(s, {outer, inner}, true, true, 1, error);
  ASSERT_TRUE(mesh) << error;
  double area = 0;
  std::map<std::array<float, 3>, size_t> points;
  std::map<std::pair<size_t, size_t>, unsigned> edges;
  for (const auto &faceTriangle : *mesh) {
    const auto &triangle = faceTriangle.positions;
    const auto a = glm::dvec2(triangle[0]), b = glm::dvec2(triangle[1]),
               c = glm::dvec2(triangle[2]);
    const auto center = (a + b + c) / 3.;
    EXPECT_FALSE(center.x > .75 && center.x < 1.25 && center.y > .75 &&
                 center.y < 1.25);
    const auto ab = b - a, ac = c - a;
    area += (ab.x * ac.y - ab.y * ac.x) / 2;
    for (unsigned i = 0; i < 3; ++i) {
      const auto key = [&](glm::vec3 p) {
        return points.try_emplace({p.x, p.y, p.z}, points.size()).first->second;
      };
      const auto x = key(triangle[i]), y = key(triangle[(i + 1) % 3]);
      ++edges[std::minmax(x, y)];
    }
  }
  EXPECT_NEAR(area, 3.75, 1e-6);
  EXPECT_EQ(std::ranges::count_if(
                edges, [](const auto &entry) { return entry.second == 1; }),
            8);
  EXPECT_TRUE(std::ranges::all_of(
      edges, [](const auto &entry) { return entry.second <= 2; }));
  auto wrong = inner;
  std::ranges::reverse(wrong);
  EXPECT_FALSE(meshIfcCurvedFace(s, {outer, wrong}, true, true, 1, error));
  EXPECT_FALSE(
      meshIfcCurvedFace(s, {outer, inner, inner}, true, true, 1, error));
  auto offSurface = outer;
  offSurface[2].z += .1f;
  EXPECT_FALSE(meshIfcCurvedFace(s, {offSurface}, true, true, 1, error));
}

TEST(IfcCurvedBrep, AmbiguousSplineChartsAndExhaustedPrecisionAreDiagnosed) {
  using container::geometry::ifc::detail::meshIfcCurvedFace;
  using container::geometry::ifc::detail::ParametricSurface;
  ParametricSurface s;
  s.point = [](double u, double v) -> std::optional<glm::dvec3> {
    return glm::dvec3((u - .5) * (u - .5), v, 0);
  };
  s.uDomain = {{0, 1}};
  s.vDomain = {{0, 1}};
  std::string error;
  EXPECT_FALSE(meshIfcCurvedFace(
      s, {{{.04f, 0, 0}, {.16f, 0, 0}, {.16f, 1, 0}, {.04f, 1, 0}}}, true, true,
      1, error));
  EXPECT_NE(error.find("Ambiguous"), std::string::npos);
  s.point = [](double u, double v) -> std::optional<glm::dvec3> {
    return glm::dvec3(1000000 + u, v, 20 * u * v);
  };
  s.uDomain = {{0, 2}};
  s.vDomain = {{0, 2}};
  s.inverse = [](glm::dvec3 p) -> std::optional<glm::dvec2> {
    return glm::dvec2(p.x - 1000000, p.y);
  };
  EXPECT_FALSE(meshIfcCurvedFace(
      s,
      {{{1000000, 0, 0}, {1000002, 0, 0}, {1000002, 2, 80}, {1000000, 2, 0}}},
      true, true, 1, error));
  EXPECT_NE(error.find("precision"), std::string::npos);
}

const std::vector<glm::dvec2> square{{-2, -2}, {2, -2}, {2, 2}, {-2, 2}};
const std::vector<glm::dvec2> hole{{-.5, -.5}, {-.5, .5}, {.5, .5}, {.5, -.5}};

TEST(IfcBrep,
     PlanarAdvancedEdgesPreserveSenseConnectivityAndSharedSubdivisions) {
  for (unsigned family : {0u, 1u, 2u}) {
    StepBuilder step;
    const auto shell = step.prism({square}, {0, 0, 0}, 4, true, false, family);
    const auto model = LoadFromStep(
        fixture(step.definitions + "#10=IFCADVANCEDBREP(" + shell + ");"));
    complete(model);
    EXPECT_NEAR(volume(model), 64, 1e-5) << family;
    watertight(model);
    for (const auto &vertex : model.vertices)
      EXPECT_GT(glm::dot(vertex.position - glm::vec3(0, 0, 2), vertex.normal),
                1.99f);
  }
}

TEST(IfcBrep, PlanarFacesRetainHolesAndBrepFaceStyles) {
  for (bool advanced : {false, true}) {
    StepBuilder step;
    const auto shell = step.prism({square, hole}, {0, 0, 0}, 4, advanced);
    const auto brep = step.add(
        std::string(advanced ? "IFCADVANCEDBREP(" : "IFCFACETEDBREP(") + shell +
        ")");
    const auto facePos =
        step.definitions.find(advanced ? "=IFCADVANCEDFACE(" : "=IFCFACE(");
    const auto faceStart = step.definitions.rfind('#', facePos);
    const auto faceRef =
        step.definitions.substr(faceStart, facePos - faceStart);
    const auto style =
        step.add("IFCSURFACESTYLE('Red',.BOTH.,(" +
                 step.add("IFCSURFACESTYLESHADING(" +
                          step.add("IFCCOLOURRGB($,1.,0.,0.)") + ",0.)") +
                 "))");
    step.add("IFCSTYLEDITEM(" + faceRef + ",(" + style + "),$)");
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    complete(model);
    EXPECT_NEAR(volume(model), 60, 1e-5);
    watertight(model);
    EXPECT_TRUE(std::ranges::any_of(model.elements, [](const auto &e) {
      return e.color.r > .99 && e.color.g < .01;
    }));
  }
}

TEST(IfcBrep, FacetedFaceBoundsAreUnorderedWithoutAnExplicitOuterBound) {
  StepBuilder step;
  const auto shell = step.prism({square, hole}, {0, 0, 0}, 4, false);
  const std::string outerType = "IFCFACEOUTERBOUND";
  size_t cursor = 0;
  while ((cursor = step.definitions.find(outerType, cursor)) !=
         std::string::npos)
    step.definitions.replace(cursor, outerType.size(), "IFCFACEBOUND");
  const std::string facePrefix = "=IFCFACE((";
  cursor = 0;
  while ((cursor = step.definitions.find(facePrefix, cursor)) !=
         std::string::npos) {
    const auto start = cursor + facePrefix.size();
    const auto end = step.definitions.find("));", start);
    const auto comma = step.definitions.find(',', start);
    if (comma < end) {
      const auto first = step.definitions.substr(start, comma - start);
      const auto second = step.definitions.substr(comma + 1, end - comma - 1);
      step.definitions.replace(start, end - start, second + "," + first);
    }
    cursor = end + 3;
  }
  const auto model = LoadFromStep(
      fixture(step.definitions + "#10=IFCFACETEDBREP(" + shell + ");"));
  complete(model);
  EXPECT_NEAR(volume(model), 60, 1e-5);
  watertight(model);
}

TEST(IfcBrep, VoidShellsRetainInwardNormalsAndWorkAsBooleanOperands) {
  for (bool advanced : {false, true}) {
    StepBuilder valid;
    const auto outerValid = valid.prism({square}, {0, 0, 0}, 4, advanced);
    auto innerSquare = square;
    for (auto &p : innerSquare)
      p *= .5;
    const auto voidShell =
        valid.prism({innerSquare}, {0, 0, 1}, 2, advanced, true);
    const auto brep =
        valid.add(std::string(advanced ? "IFCADVANCEDBREPWITHVOIDS("
                                       : "IFCFACETEDBREPWITHVOIDS(") +
                  outerValid + ",(" + voidShell + "))");
    const auto model = LoadFromStep(fixture(valid.definitions, brep));
    complete(model);
    EXPECT_NEAR(volume(model), 56, 1e-5);
    watertight(model);
    unsigned innerNormals = 0;
    for (const auto &vertex : model.vertices)
      if (std::abs(vertex.position.x) <= 1 &&
          std::abs(vertex.position.y) <= 1 && vertex.position.z >= 1 &&
          vertex.position.z <= 3) {
        ++innerNormals;
        EXPECT_LT(glm::dot(vertex.position - glm::vec3(0, 0, 2), vertex.normal),
                  -.99f);
      }
    EXPECT_GT(innerNormals, 0u);
    const auto cut = valid.add(
        "IFCEXTRUDEDAREASOLID(" +
        valid.add("IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.)") + ",$,$,2.)");
    const auto boolean =
        valid.add("IFCBOOLEANRESULT(.DIFFERENCE.," + brep + "," + cut + ")");
    const auto clipped = LoadFromStep(fixture(valid.definitions, boolean));
    complete(clipped);
    // Above z=2 the remaining 32 m^3 contains half of the 8 m^3 cavity.
    // Count the draw ranges selected by the representation.
    double drawnVolume = 0;
    for (const auto &element : clipped.elements) {
      const auto range =
          std::ranges::find(clipped.meshRanges, element.meshId,
                            &container::geometry::dotbim::MeshRange::meshId);
      ASSERT_NE(range, clipped.meshRanges.end());
      for (size_t i = range->firstIndex;
           i < size_t(range->firstIndex) + range->indexCount; i += 3) {
        const glm::dvec3 a = clipped.vertices[clipped.indices[i]].position;
        const glm::dvec3 b = clipped.vertices[clipped.indices[i + 1]].position;
        const glm::dvec3 c = clipped.vertices[clipped.indices[i + 2]].position;
        drawnVolume += glm::dot(a, glm::cross(b, c)) / 6;
      }
    }
    EXPECT_NEAR(drawnVolume, 28, 1e-5);
  }
}

TEST(IfcBrep, MultipleDisjointCavitiesRetainTheirAuthoredSurfaces) {
  for (bool advanced : {false, true}) {
    StepBuilder step;
    auto small = square;
    for (auto &p : small)
      p *= .25;
    const auto outer = step.prism({square}, {0, 0, 0}, 4, advanced);
    const auto first = step.prism({small}, {-1, 0, 1.5}, 1, advanced, true);
    const auto second = step.prism({small}, {1, 0, 1.5}, 1, advanced, true);
    const auto brep =
        step.add(std::string(advanced ? "IFCADVANCEDBREPWITHVOIDS("
                                      : "IFCFACETEDBREPWITHVOIDS(") +
                 outer + ",(" + first + "," + second + "))");
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    complete(model);
    EXPECT_NEAR(volume(model), 62, 1e-5);
    watertight(model);
  }
}

TEST(IfcBrep, MillimetreCavitiesPreserveProjectUnitsAndPhysicalVolume) {
  StepBuilder step;
  auto large = square, small = square;
  for (auto &p : large)
    p *= 1000;
  for (auto &p : small)
    p *= 500;
  const auto outer = step.prism({large}, {0, 0, 0}, 4000, true);
  const auto inner = step.prism({small}, {0, 0, 1000}, 2000, true, true);
  const auto brep =
      step.add("IFCADVANCEDBREPWITHVOIDS(" + outer + ",(" + inner + "))");
  auto source = fixture(step.definitions, brep);
  source.replace(source.find(".LENGTHUNIT.,$,.METRE."),
                 std::string(".LENGTHUNIT.,$,.METRE.").size(),
                 ".LENGTHUNIT.,.MILLI.,.METRE.");
  const auto model = LoadFromStep(source);
  complete(model);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.unitMetadata.metersPerUnit, .001, 1e-9);
  double physicalVolume = 0;
  const auto &transform = model.elements.front().transform;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    const auto world = [&](size_t j) {
      return glm::dvec3(
          transform * glm::vec4(model.vertices[model.indices[j]].position, 1));
    };
    const auto a = world(i), b = world(i + 1), c = world(i + 2);
    physicalVolume += glm::dot(a, glm::cross(b, c)) / 6;
  }
  EXPECT_NEAR(physicalVolume, 56, 2e-5);
  watertight(model);
}

TEST(IfcBrep, InvalidVoidsAndOpenShellsRejectWithoutPartialBuffers) {
  for (unsigned invalid = 0; invalid < 7; ++invalid) {
    StepBuilder step;
    const auto outer = step.prism({square}, {0, 0, 0}, 4, false);
    auto innerSquare = square;
    for (auto &p : innerSquare)
      p *= invalid == 1 ? 1 : .5;
    const auto inner = step.prism({innerSquare}, {invalid == 2 ? 3. : 0., 0, 1},
                                  2, false, invalid != 0);
    std::vector<std::string> voids{inner};
    if (invalid == 3)
      voids.push_back(inner);
    if (invalid == 4)
      voids.push_back(outer);
    if (invalid == 5 || invalid == 6) {
      for (auto &p : innerSquare)
        p *= invalid == 5 ? .5 : 1;
      voids.push_back(step.prism(
          {innerSquare}, {invalid == 6 ? 1. : 0., 0, 1.5}, 1, false, true));
    }
    const auto model =
        LoadFromStep(fixture(step.definitions + "#10=IFCFACETEDBREPWITHVOIDS(" +
                             outer + ",(" + step.refs(voids) + "));"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed)
        << invalid;
    EXPECT_TRUE(model.vertices.empty()) << invalid;
    EXPECT_TRUE(model.meshRanges.empty()) << invalid;
    ASSERT_FALSE(model.importReport.diagnostics.empty());
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 10u);
  }
  StepBuilder step;
  const auto outer = step.prism({square}, {0, 0, 0}, 4, false);
  const auto listStart = step.definitions.find("=IFCCLOSEDSHELL((") + 17;
  const auto comma = step.definitions.find(',', listStart);
  step.definitions.erase(listStart, comma - listStart + 1);
  const auto model = LoadFromStep(
      fixture(step.definitions + "#10=IFCFACETEDBREP(" + outer + ");"));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(model.vertices.empty());
}

TEST(IfcBrep, InvalidAdvancedSurfaceAndEdgeTopologyAreDiagnosed) {
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    StepBuilder step;
    const auto shell = step.prism({square}, {0, 0, 0}, 4, true);
    if (invalid == 0) {
      const auto p = step.definitions.find("=IFCADVANCEDFACE(");
      const auto s = step.definitions.find(",.T.);", p);
      step.definitions.replace(s + 2, 1, "F");
    } else if (invalid == 1) {
      const auto p = step.definitions.find("=IFCORIENTEDEDGE(");
      const auto s = step.definitions.find(",.T.);", p);
      step.definitions.replace(s + 2, 1, "F");
    } else if (invalid == 2) {
      const auto p = step.definitions.find("=IFCPLANE(");
      step.definitions.replace(p + 1, 8, "IFCCYLINDRICALSURFACE");
    } else if (invalid == 3) {
      const auto p = step.definitions.find("=IFCEDGECURVE(");
      const auto s = step.definitions.find(",.T.);", p);
      step.definitions.replace(s + 2, 1, "F");
    } else {
      const auto p = step.definitions.find("=IFCVERTEXPOINT(");
      const auto s = step.definitions.find('#', p);
      const auto e = step.definitions.find(')', s);
      step.definitions.replace(s, e - s, "#999999");
    }
    const auto model = LoadFromStep(
        fixture(step.definitions + "#10=IFCADVANCEDBREP(" + shell + ");"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed)
        << invalid;
    EXPECT_TRUE(model.vertices.empty()) << invalid;
    ASSERT_FALSE(model.importReport.diagnostics.empty());
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 10u);
    EXPECT_NE(model.importReport.diagnostics.front().reason.find("Face #"),
              std::string::npos);
  }
}
TEST(IfcBrep, BuildingSmartLegacyBasinReportsItsInvalidShell) {
  const char *root = std::getenv("CONTAINER_IFC_BREP_SAMPLE_ROOT");
  if (!root)
    GTEST_SKIP() << "Set CONTAINER_IFC_BREP_SAMPLE_ROOT to the buildingSMART "
                    "basin fixtures";
  const auto faceted = container::geometry::ifc::LoadFromFile(
      std::filesystem::path(root) / "basin-faceted-brep.ifc");
  const auto tessellated = container::geometry::ifc::LoadFromFile(
      std::filesystem::path(root) / "basin-tessellation.ifc");
  // The archived 2014 shell has 120 repeated directed boundary edges and
  // three unmatched edges, before and after triangulation. Preserve the
  // source winding and report this invalid solid rather than repairing it.
  EXPECT_EQ(faceted.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(faceted.vertices.empty());
  ASSERT_EQ(faceted.importReport.diagnostics.size(), 1u);
  const auto &diagnostic = faceted.importReport.diagnostics.front();
  EXPECT_EQ(diagnostic.entityId, 852u);
  EXPECT_NE(diagnostic.reason.find("Shell #851"), std::string::npos);
  EXPECT_NE(diagnostic.reason.find("closed manifold"), std::string::npos);
  // A tessellated face set can represent an open surface independently of
  // the solid-shell requirements. Its drawable result remains available.
  EXPECT_EQ(tessellated.importReport.completeness,
            ImportCompleteness::Complete);
  EXPECT_EQ(tessellated.importReport.importedProductCount, 1u);
  EXPECT_FALSE(tessellated.indices.empty());
}
} // namespace
