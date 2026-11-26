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
#include <initializer_list>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <span>
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
std::string fixture(const std::string &definitions, std::string item = "#10",
                    std::string_view representation = "'Body','Brep'") {
  return "ISO-10303-21; DATA; #1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);" +
         definitions + "#900001=IFCSHAPEREPRESENTATION($," +
         std::string(representation) + ",(" + item +
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

void expectProfileBounds(const Model &model, glm::dvec3 minimum,
                         glm::dvec3 maximum, double divisor = 1) {
  ASSERT_FALSE(model.vertices.empty());
  glm::dvec3 actualMin(std::numeric_limits<double>::infinity());
  glm::dvec3 actualMax(-actualMin);
  for (const auto &vertex : model.vertices) {
    const glm::dvec3 point = glm::dvec3(vertex.position) / divisor;
    actualMin = glm::min(actualMin, point);
    actualMax = glm::max(actualMax, point);
  }
  EXPECT_LT(glm::length(actualMin - minimum), 1e-5);
  EXPECT_LT(glm::length(actualMax - maximum), 1e-5);
}

void expectProfileArc(const Model &model, glm::dvec2 center, double radius,
                      glm::dvec2 toward) {
  SCOPED_TRACE(::testing::Message() << "Arc center " << center.x << ", "
                                    << center.y << " with radius " << radius);
  const auto bisector = glm::normalize(toward);
  EXPECT_TRUE(std::any_of(
      model.vertices.begin(), model.vertices.end(), [&](const auto &vertex) {
        const auto delta = glm::dvec2(vertex.position) - center;
        return std::abs(glm::length(delta) - radius) < 1e-5 &&
               glm::dot(delta, bisector) > .92 * radius;
      }));
}

std::string degrees() {
  return "#70=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.);"
         "#71=IFCMEASUREWITHUNIT(IFCPLANEANGLEMEASURE(0.017453292519943295),#"
         "70);"
         "#72=IFCCONVERSIONBASEDUNIT($,.PLANEANGLEUNIT.,'Degree',#71);"
         "#73=IFCUNITASSIGNMENT((#1,#72));"
         "#74=IFCPROJECT('project',$,$,$,$,$,$,(),#73);";
}

void exportProfileFixture(const char *name, const std::string &source) {
  if (const auto *root = std::getenv("CONTAINER_IFC_PROFILE_FIXTURE_ROOT")) {
    std::filesystem::create_directories(root);
    std::ofstream stream(std::filesystem::path(root) /
                         (std::string(name) + ".ifc"));
    stream << source;
    ASSERT_TRUE(stream.good());
  }
}

TEST(IfcStructuralProfiles, DerivedAndMirroredAreaProfilesRetainPlacedBounds) {
  for (bool millimetres : {false, true})
    for (bool mirror : {false, true})
      for (bool nested : {false, true})
        for (bool negative : {false, true}) {
          const double m = millimetres ? 1000 : 1;
          const std::string definitions =
              "#2=IFCCARTESIANPOINT((" + number(m) + "," + number(2 * m) +
              ")); #3=IFCDIRECTION((0.,1.)); #4=IFCAXIS2PLACEMENT2D(#2,#3);"
              "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,#4," +
              number(4 * m) + "," + number(2 * m) +
              "); #6=IFCCARTESIANPOINT((" + number(4 * m) + "," + number(-m) +
              ")); #7=IFCDIRECTION((1.,0.));"
              "#8=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM(#3,#7,#6,3.,0."
              "5);"
              "#9=IFCDERIVEDPROFILEDEF(.AREA.,$,#5,#8,$);"
              "#11=IFCMIRROREDPROFILEDEF(.AREA.,$,#9,*,$);"
              "#12=IFCCARTESIANPOINT((" +
              number(m) + "," + number(-2 * m) +
              ")); #13=IFCCARTESIANTRANSFORMATIONOPERATOR2D($,$,#12,2.);"
              "#14=IFCDERIVEDPROFILEDEF(.AREA.,$," +
              std::string(mirror ? "#11" : "#9") +
              ",#13,$);"
              "#15=IFCDIRECTION((0.,0.," +
              std::string(negative ? "-1." : "1.") +
              ")); #10=IFCEXTRUDEDAREASOLID(" +
              std::string(nested   ? "#14"
                          : mirror ? "#11"
                                   : "#9") +
              ",$,#15," + number(3 * m) + ");";
          auto source = fixture(definitions);
          if (millimetres) {
            const auto at = source.find(".LENGTHUNIT.,$,.METRE.");
            ASSERT_NE(at, std::string::npos);
            source.replace(at, std::string(".LENGTHUNIT.,$,.METRE.").size(),
                           ".LENGTHUNIT.,.MILLI.,.METRE.");
          }
          const auto model = LoadFromStep(source);
          complete(model);
          EXPECT_NEAR(volume(model) / (m * m * m), nested ? 144 : 36, 2e-5);
          watertight(model);
          glm::dvec3 low(std::numeric_limits<double>::infinity()), high(-low);
          for (const auto &vertex : model.vertices) {
            const glm::dvec3 p = glm::dvec3(vertex.position) / m;
            low = glm::min(low, p);
            high = glm::max(high, p);
          }
          const double xmin = nested ? (mirror ? -11 : 9) : (mirror ? -6 : 4),
                       xmax = nested ? (mirror ? -7 : 13) : (mirror ? -4 : 6);
          EXPECT_EQ(low, glm::dvec3(xmin, nested ? -4 : -1, negative ? -3 : 0));
          EXPECT_EQ(high, glm::dvec3(xmax, nested ? 8 : 5, negative ? 0 : 3));
          if (!millimetres && !nested && !negative)
            exportProfileFixture(
                mirror ? "mirrored-rectangle" : "derived-rectangle", source);
        }
}

TEST(IfcStructuralProfiles,
     ScaledCurvedAreaProfilesRetainChordAccuracyAndVoids) {
  for (const auto &[sx, sy] :
       std::array<std::pair<double, double>, 2>{{{100, 2}, {3, 1.5}}})
    for (bool mirror : {false, true}) {
      for (bool indexed : {false, true}) {
        const std::string parent =
            indexed ? "#40=IFCCARTESIANPOINTLIST2D(((1.,0.),(0.,1.),(-1.,0.),("
                      "0.,-1.)));"
                      "#41=IFCINDEXEDPOLYCURVE(#40,(IFCARCINDEX((1,2,3)),"
                      "IFCARCINDEX((3,4,1))),.F.);"
                      "#42=IFCCARTESIANPOINTLIST2D(((0.75,0.),(0.,0.75),(-0.75,"
                      "0.),(0.,-0.75)));"
                      "#43=IFCINDEXEDPOLYCURVE(#42,(IFCARCINDEX((1,2,3)),"
                      "IFCARCINDEX((3,4,1))),.F.);"
                      "#5=IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,$,#41,(#43));"
                    : "#5=IFCCIRCLEHOLLOWPROFILEDEF(.AREA.,$,$,1.,0.25);";
        const auto source = fixture(
            "#2=IFCCARTESIANPOINT((0.,0.));"
            "#3=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#2," +
            number(sx) + "," + number(sy) + ");" + parent +
            "#9=IFCDERIVEDPROFILEDEF(.AREA.,$,#5,#3,$);"
            "#11=IFCMIRROREDPROFILEDEF(.AREA.,$,#9,*,$);"
            "#10=IFCEXTRUDEDAREASOLID(" +
            std::string(mirror ? "#11" : "#9") + ",$,$,3.);");
        const auto model = LoadFromStep(source);
        complete(model);
        watertight(model);
        EXPECT_NEAR(volume(model), pi * (1 - .75 * .75) * sx * sy * 3, .015);
        std::map<double, glm::dvec2> outer;
        for (const auto &vertex : model.vertices) {
          const glm::dvec3 p = vertex.position;
          const double rho = p.x * p.x / (sx * sx) + p.y * p.y / (sy * sy);
          EXPECT_TRUE(std::abs(rho - 1) < 1e-6 ||
                      std::abs(rho - .75 * .75) < 1e-6);
          if (p.z == 0 && rho > .9)
            outer[std::atan2(p.y / sy, p.x / sx)] = {p.x, p.y};
        }
        ASSERT_GT(outer.size(), 16u);
        for (auto a = outer.begin(); a != outer.end(); ++a) {
          auto b = std::next(a);
          const bool seam = b == outer.end();
          if (seam)
            b = outer.begin();
          const double theta = (a->first + b->first + (seam ? 2 * pi : 0)) / 2;
          const glm::dvec2 exact(sx * std::cos(theta), sy * std::sin(theta));
          const auto d = b->second - a->second;
          const auto nearest =
              a->second +
              d * std::clamp(glm::dot(exact - a->second, d) / glm::dot(d, d),
                             0., 1.);
          EXPECT_LE(glm::length(exact - nearest), .00101);
        }
        for (size_t i = 0; i < model.indices.size(); i += 3) {
          const glm::dvec3 a = model.vertices.at(model.indices[i]).position,
                           b = model.vertices.at(model.indices[i + 1]).position,
                           c = model.vertices.at(model.indices[i + 2]).position;
          if (a.z == b.z && b.z == c.z)
            continue;
          const auto center = (a + b + c) / 3.;
          const glm::dvec3 radial(center.x / (sx * sx), center.y / (sy * sy),
                                  0);
          const auto normal = glm::cross(b - a, c - a);
          const double rho = a.x * a.x / (sx * sx) + a.y * a.y / (sy * sy);
          EXPECT_GT(glm::dot(normal, radial) * (rho > .9 ? 1 : -1), 0);
        }
        if (sx == 3 && mirror && !indexed)
          exportProfileFixture("mirrored-hollow", source);
      }
    }
}

TEST(IfcStructuralProfiles, DerivedStructuralProfilesAndVoidsSupportBooleans) {
  for (bool voids : {false, true}) {
    const std::string parent =
        voids
            ? "#40=IFCCARTESIANPOINT((-4.,-4.)); "
              "#41=IFCCARTESIANPOINT((4.,-4.));"
              "#42=IFCCARTESIANPOINT((4.,4.)); #43=IFCCARTESIANPOINT((-4.,4.));"
              "#44=IFCPOLYLINE((#40,#41,#42,#43,#40));"
              "#45=IFCCARTESIANPOINT((-1.,-1.)); "
              "#46=IFCCARTESIANPOINT((1.,-1.));"
              "#47=IFCCARTESIANPOINT((1.,1.)); #48=IFCCARTESIANPOINT((-1.,1.));"
              "#49=IFCPOLYLINE((#45,#46,#47,#48,#45));"
              "#5=IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,$,#44,(#49));"
            : "#5=IFCLSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,$,$,$);";
    const auto source = fixture(
        parent +
        "#2=IFCCARTESIANPOINT((0.,0.));"
        "#3=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#2,2.,3.);"
        "#9=IFCDERIVEDPROFILEDEF(.AREA.,$,#5,#3,$);"
        "#11=IFCMIRROREDPROFILEDEF(.AREA.,$,#9,*,$);"
        "#12=IFCEXTRUDEDAREASOLID(#11,$,$,2.);"
        "#20=IFCCARTESIANPOINT((0.,-10.5)); #21=IFCAXIS2PLACEMENT2D(#20,$);"
        "#22=IFCRECTANGLEPROFILEDEF(.AREA.,$,#21,2.,1.);"
        "#23=IFCEXTRUDEDAREASOLID(#22,$,$,2.);"
        "#10=IFCBOOLEANRESULT(.DIFFERENCE.,#12,#23);");
    const auto model = LoadFromStep(source);
    complete(model);
    EXPECT_NEAR(volume(model), (voids ? 60 : 13) * 6 * 2 - 4, 1e-5);
    watertight(model);
    exportProfileFixture(voids ? "derived-void-cut" : "derived-L-cut", source);
  }
}

TEST(IfcStructuralProfiles, InvalidDerivedAreaProfilesRejectAtomically) {
  const std::string definitions =
      "#2=IFCCARTESIANPOINT((0.,0.));"
      "#3=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#2,2.,3.);"
      "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,2.);"
      "#9=IFCDERIVEDPROFILEDEF(.AREA.,$,#5,#3,$);"
      "#10=IFCEXTRUDEDAREASOLID(#9,$,$,3.);";
  std::vector<std::string> invalid;
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"#5,#3,$", "#9,#3,$"},
           {"#5,#3,$", "#99,#3,$"},
           {"IFCDERIVEDPROFILEDEF(.AREA.", "IFCDERIVEDPROFILEDEF(.CURVE."},
           {"#2,2.,3.", "#2,-2.,3."},
           {"#2,2.,3.", "#2,2.,0."},
           {"((0.,0.))", "((0.,0.,0.))"},
           {"IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM",
            "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM"}}) {
    auto source = definitions;
    const auto at = source.find(from);
    ASSERT_NE(at, std::string::npos);
    source.replace(at, from.size(), to);
    invalid.push_back(source);
  }
  invalid.push_back(definitions +
                    "#11=IFCMIRROREDPROFILEDEF(.AREA.,$,#5,#3,$);");
  invalid.back().replace(invalid.back().find("SOLID(#9"),
                         std::string("SOLID(#9").size(), "SOLID(#11");
  auto excessive = definitions;
  for (unsigned i = 100; i < 170; ++i)
    excessive += "#" + std::to_string(i) + "=IFCDERIVEDPROFILEDEF(.AREA.,$,#" +
                 std::to_string(i == 169 ? 5 : i + 1) + ",#3,$);";
  excessive.replace(excessive.find("#5,#3,$"), std::string("#5,#3,$").size(),
                    "#100,#3,$");
  invalid.push_back(excessive);
  for (const auto &source : invalid) {
    const auto model = LoadFromStep(fixture(source));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 10u);
    EXPECT_NE(model.importReport.diagnostics.front().reason.find("profile"),
              std::string::npos);
  }
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

TEST(IfcStructuralProfiles, IShapeFilletsCanMeetAtWebAndFlangeLimits) {
  constexpr double extrusion = 2.;
  for (const auto &[width, depth, tangentX, tangentY] :
       std::array<std::array<double, 4>, 2>{{{10., 6., 1., 0.},
                                            {6., 8., 3., 3.}}}) {
    SCOPED_TRACE(::testing::Message() << width << " x " << depth);
    const auto source = [&](double radius) {
      return fixture("#5=IFCISHAPEPROFILEDEF(.AREA.,$,$," + number(width) +
                         "," + number(depth) + ",2.,1.," + number(radius) +
                         ",0.,0.); #10=IFCEXTRUDEDAREASOLID(#5,$,$," +
                         number(extrusion) + ");",
                     "#10", "'Body','SweptSolid'");
    };
    const auto model = LoadFromStep(source(2.));
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    expectProfileBounds(model, {-width / 2, -depth / 2, 0.},
                        {width / 2, depth / 2, extrusion});

    for (double x : {-tangentX, tangentX})
      for (double y : {-tangentY, tangentY}) {
        const glm::vec3 tangent(x, y, extrusion);
        EXPECT_TRUE(std::ranges::any_of(model.vertices, [&](const auto &v) {
          return v.position == tangent;
        })) << "Missing tangent at " << x << ", " << y;
      }

    // Four concave fillets each add a radius-2 square minus a quarter disk.
    const double area = 2 * width + 2 * (depth - 2) + 16 - 4 * pi;
    double capArea = 0.;
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      const glm::dvec3 a(model.vertices.at(model.indices.at(i)).position);
      const glm::dvec3 b(model.vertices.at(model.indices.at(i + 1)).position);
      const glm::dvec3 c(model.vertices.at(model.indices.at(i + 2)).position);
      if (a.z == extrusion && b.z == extrusion && c.z == extrusion)
        capArea += glm::cross(b - a, c - a).z / 2;
    }
    EXPECT_NEAR(capArea, area, .01);
    EXPECT_NEAR(volume(model), area * extrusion, .02);

    const auto overlap = LoadFromStep(source(2.01));
    EXPECT_EQ(overlap.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(overlap.vertices.empty());
    EXPECT_TRUE(overlap.indices.empty());
    EXPECT_TRUE(overlap.meshRanges.empty());
  }
}

TEST(IfcStructuralProfiles, CShapeBendsPreserveWallThicknessAndGirth) {
  constexpr double corner = 1 - pi / 4;
  for (const std::string radius : {"$", "0.", "0.4"}) {
    SCOPED_TRACE(radius);
    const auto source =
        fixture("#5=IFCCSHAPEPROFILEDEF(.AREA.,$,$,10.,6.,0.5,2.," + radius +
                "); #10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);");
    const auto model = LoadFromStep(source);
    complete(model);
    watertight(model);
    expectProfileBounds(model, {-3, -5, 0}, {3, 5, 3});

    // The sharp section consists of a back web, two shelves and two lips.
    // Concentric bends add the inner corner deficit and subtract the outer one.
    const double inner = radius == "0.4" ? .4 : 0.;
    const double outer = radius == "$" ? 0. : inner + .5;
    const double sharpArea = .5 * 10 + 2 * .5 * (6 - .5) + 2 * .5 * (2 - .5);
    EXPECT_NEAR(volume(model),
                (sharpArea + 4 * corner * (inner * inner - outer * outer)) * 3,
                outer > 0 ? .03 : 1e-5);
    if (outer > 0) {
      for (double x : {-1., 1.})
        for (double y : {-1., 1.}) {
          const glm::dvec2 center(x * (3 - outer), y * (5 - outer));
          expectProfileArc(model, center, outer, {x, y});
          if (inner > 0)
            expectProfileArc(model, center, inner, {x, y});
        }
    }
    if (inner > 0)
      exportProfileFixture("area-c-channel", source);
  }
}

TEST(IfcStructuralProfiles, ZShapeFlangeWidthRunsToOppositeWebFace) {
  const auto source =
      fixture("#5=IFCZSHAPEPROFILEDEF(.AREA.,$,$,10.,5.,2.,1.,0.6,0.25);"
              "#10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);");
  const auto model = LoadFromStep(source);
  complete(model);
  watertight(model);
  expectProfileBounds(model, {-4, -5, 0}, {4, 5, 3});
  const double sharpArea = 2 * 10 + 2 * (5 - 2);
  EXPECT_NEAR(volume(model),
              (sharpArea + 2 * (1 - pi / 4) * (.6 * .6 - .25 * .25)) * 3, .02);
  for (double sign : {-1., 1.}) {
    expectProfileArc(model, sign * glm::dvec2(-1.6, 3.4), .6,
                     sign * glm::dvec2(1, 1));
    expectProfileArc(model, sign * glm::dvec2(3.75, -4.25), .25,
                     sign * glm::dvec2(1, 1));
  }
  exportProfileFixture("area-z-section", source);
}

TEST(IfcStructuralProfiles, NestedZShapePlacementRetainsMillimetreBounds) {
  auto source = fixture(
      "#2=IFCCARTESIANPOINT((1000.,2000.)); #3=IFCDIRECTION((0.,1.));"
      "#4=IFCAXIS2PLACEMENT2D(#2,#3);"
      "#5=IFCZSHAPEPROFILEDEF(.AREA.,$,#4,10000.,5000.,2000.,1000.,$,$);"
      "#6=IFCMIRROREDPROFILEDEF(.AREA.,$,#5,*,$);"
      "#7=IFCCARTESIANPOINT((8000.,-3000.));"
      "#8=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#7,1.5,0.5);"
      "#9=IFCDERIVEDPROFILEDEF(.AREA.,$,#6,#8,$);"
      "#11=IFCDIRECTION((0.,0.,-1.));"
      "#10=IFCEXTRUDEDAREASOLID(#9,$,#11,3000.);");
  const auto unit = source.find(".LENGTHUNIT.,$,.METRE.");
  ASSERT_NE(unit, std::string::npos);
  source.replace(unit, std::string_view(".LENGTHUNIT.,$,.METRE.").size(),
                 ".LENGTHUNIT.,.MILLI.,.METRE.");
  const auto model = LoadFromStep(source);
  complete(model);
  watertight(model);
  // In metres the 2D transformations map (x,y) to (6.5+1.5*y,-2+.5*x).
  expectProfileBounds(model, {-1, -4, -3}, {14, 0, 0}, 1000);
  EXPECT_NEAR(volume(model) / 1e9, 26 * 3 * .75, 1e-5);
}

TEST(IfcStructuralProfiles, TShapeRoundsWebAndFlangeEdgesIndependently) {
  const auto source = fixture(
      "#5=IFCTSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,2.,1.,0.55,0.25,0.35,$,$);"
      "#10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);");
  const auto model = LoadFromStep(source);
  complete(model);
  watertight(model);
  expectProfileBounds(model, {-3, -4, 0}, {3, 4, 3});
  const double sharpArea = 6 + 7 * 2;
  EXPECT_NEAR(
      volume(model),
      (sharpArea + 2 * (1 - pi / 4) * (.55 * .55 - .25 * .25 - .35 * .35)) * 3,
      .02);
  for (double sign : {-1., 1.}) {
    expectProfileArc(model, {sign * 1.55, 2.45}, .55, {-sign, 1});
    expectProfileArc(model, {sign * 2.75, 3.25}, .25, {sign, -1});
    expectProfileArc(model, {sign * .65, -3.65}, .35, {sign, -1});
  }
  exportProfileFixture("area-t-section", source);
}

TEST(IfcStructuralProfiles, TShapeSignedSlopesHonorReferenceThicknesses) {
  for (const auto &[web, flange] :
       std::array<std::pair<double, double>, 2>{{{.08, -.11}, {-.07, .12}}}) {
    for (bool degreeUnits : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << web << ", " << flange << ", degrees " << degreeUnits);
      const double angleUnit = degreeUnits ? 180 / pi : 1;
      const auto source =
          fixture("#5=IFCTSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,2.,1.,$,$,$," +
                  number(std::atan(web) * angleUnit) + "," +
                  number(std::atan(flange) * angleUnit) +
                  "); #10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);" +
                  (degreeUnits ? degrees() : ""));
      const auto model = LoadFromStep(source);
      complete(model);
      watertight(model);
      expectProfileBounds(model, {-3, -4, 0}, {3, 4, 3});

      // The thickness markers in the IFC figure sit at y=0 on the web and
      // one quarter of the flange width from the tip, at x=1.5 here.
      const double yRoot = (3 - flange / 2) / (1 - web * flange);
      const double xRoot = 1 + yRoot * web;
      const double xBottom = 1 - 4 * web;
      const double yTip = 3 + 1.5 * flange;
      const double webArea = (4 + yRoot) * (xBottom + xRoot);
      const double centralFlangeArea = (4 - yRoot) * 2 * xRoot;
      const double overhangArea = (3 - xRoot) * (8 - yRoot - yTip);
      EXPECT_NEAR(volume(model),
                  (webArea + centralFlangeArea + overhangArea) * 3, 2e-5);
    }
  }
}

TEST(IfcStructuralProfiles,
     AsymmetricIShapeUsesIndependentFlangeSlopesAndRadii) {
  constexpr double bottomSlope = .12, topSlope = -.18;
  constexpr double bottomRoot = .7, bottomEdge = .18;
  constexpr double topRoot = .45, topEdge = .27;
  for (bool degreeUnits : {false, true}) {
    const double angleUnit = degreeUnits ? 180 / pi : 1;
    const auto source =
        fixture("#5=IFCASYMMETRICISHAPEPROFILEDEF(.AREA.,$,$,9.,10.,2.,1.4,0.7,"
                "6.,1.,0.45,0.18," +
                number(std::atan(bottomSlope) * angleUnit) + ",0.27," +
                number(std::atan(topSlope) * angleUnit) +
                "); #10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);" +
                (degreeUnits ? degrees() : ""));
    const auto model = LoadFromStep(source);
    complete(model);
    watertight(model);
    expectProfileBounds(model, {-4.5, -5, 0}, {4.5, 5, 3});

    // Begin with the whole web and the uniform-thickness flange overhangs.
    // Each sloped flange adds two triangular wedges. Circular fillets add
    // tangent-minus-sector areas at the web roots and remove them at the tips.
    const double sharpArea = 2 * 10 + (9 - 2) * 1.4 + (6 - 2) * 1. +
                             3.5 * 3.5 * bottomSlope + 2 * 2 * topSlope;
    const auto cornerFactor = [](double taper) {
      const double halfTurn = (pi / 2 - std::atan(taper)) / 2;
      return std::tan(halfTurn) - halfTurn;
    };
    const double area =
        sharpArea +
        2 * cornerFactor(bottomSlope) *
            (bottomRoot * bottomRoot - bottomEdge * bottomEdge) +
        2 * cornerFactor(topSlope) * (topRoot * topRoot - topEdge * topEdge);
    EXPECT_NEAR(volume(model), area * 3, .025);

    const auto checkFlange = [&](double halfWidth, double thickness,
                                 double taper, double root, double edge,
                                 double ySign) {
      const auto underside = [&](double x) {
        return -5 + thickness + taper * (halfWidth - x);
      };
      const double perpendicular = std::hypot(1., taper);
      const glm::dvec2 rootCenter(1 + root,
                                  underside(1 + root) + root * perpendicular);
      const glm::dvec2 edgeCenter(
          halfWidth - edge, underside(halfWidth - edge) - edge * perpendicular);
      for (double side : {-1., 1.}) {
        const glm::dvec2 orient(side, ySign);
        expectProfileArc(model, orient * rootCenter, root,
                         orient * (glm::dvec2(1, underside(1)) - rootCenter));
        expectProfileArc(model, orient * edgeCenter, edge,
                         orient * (glm::dvec2(halfWidth, underside(halfWidth)) -
                                   edgeCenter));
      }
    };
    checkFlange(4.5, 1.4, bottomSlope, bottomRoot, bottomEdge, 1);
    checkFlange(3, 1, topSlope, topRoot, topEdge, -1);
    if (!degreeUnits)
      exportProfileFixture("area-asymmetric-i-section", source);
  }
}

TEST(IfcStructuralProfiles,
     AsymmetricIShapeReadsIfc2x3DefaultsAndCentroidMetadata) {
  for (bool explicitTop : {false, true}) {
    SCOPED_TRACE(explicitTop);
    const std::string upperFlange = explicitTop ? "1.,0.35," : "$,$,";
    auto source = fixture(
        "#2=IFCCARTESIANPOINT((0.,0.)); #4=IFCAXIS2PLACEMENT2D(#2,$);"
        "#5=IFCASYMMETRICISHAPEPROFILEDEF(.AREA.,$,#4,9.,10.,2.,1.5,0.6,6.," +
        upperFlange + "2.4); #10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);");
    source.insert(source.find("DATA;"),
                  "HEADER; FILE_SCHEMA(('IFC2X3')); ENDSEC; ");
    const auto model = LoadFromStep(source);
    complete(model);
    watertight(model);
    expectProfileBounds(model, {-4.5, -5, 0}, {4.5, 5, 3});

    const double topThickness = explicitTop ? 1. : 1.5;
    const double topRadius = explicitTop ? .35 : .6;
    const double area = 2 * 10 + (9 - 2) * 1.5 + (6 - 2) * topThickness +
                        2 * (1 - pi / 4) * (.6 * .6 + topRadius * topRadius);
    EXPECT_NEAR(volume(model), area * 3, .015);
    for (double side : {-1., 1.}) {
      expectProfileArc(model, {side * 1.6, -2.9}, .6, {-side, -1});
      expectProfileArc(model,
                       {side * (1 + topRadius), 5 - topThickness - topRadius},
                       topRadius, {-side, 1});
    }
  }
}

std::string craneRailFixture(std::string_view type, const std::string &fields,
                            double scale = 1.,
                            std::string_view schema = "IFC2X3") {
  auto source = fixture(
      "#2=IFCCARTESIANPOINT((" + number(7 * scale) + "," +
          number(-4 * scale) + ")); #3=IFCDIRECTION((0.,1.));"
          "#4=IFCAXIS2PLACEMENT2D(#2,#3); #5=IFCCRANERAIL" +
          std::string(type) + "SHAPEPROFILEDEF(" + fields +
          "); #10=IFCEXTRUDEDAREASOLID(#5,$,$," + number(2 * scale) + ");",
      "#10", "'Body','SweptSolid'");
  if (!schema.empty())
    source.insert(source.find("DATA;"),
                  "HEADER; FILE_SCHEMA(('" + std::string(schema) + "')); ENDSEC; ");
  if (scale == 1000.) {
    constexpr std::string_view metre = ".LENGTHUNIT.,$,.METRE.";
    source.replace(source.find(metre), metre.size(), ".LENGTHUNIT.,.MILLI.,.METRE.");
  }
  return source;
}

TEST(IfcStructuralProfiles, Ifc2x3CraneRailsPreserveNominalStationsAndCrown) {
  for (bool typeA : {false, true})
    for (double scale : {1., 1000.}) {
      const double radius = scale == 1. ? 0. : .60;
      SCOPED_TRACE(::testing::Message() << "Type " << (typeA ? 'A' : 'F')
                                       << ", length multiplier " << scale);
      const auto fields = [scale](std::initializer_list<double> dimensions) {
        std::string result;
        for (double length : dimensions) {
          if (!result.empty())
            result += ',';
          result += number(length * scale);
        }
        return result;
      };
      const std::string radiusField = radius == 0 ? "$" : number(radius * scale);
      const auto source = craneRailFixture(
          typeA ? "A" : "F",
          ".AREA.,$,#4," + fields(typeA ? std::initializer_list<double>{8., 8.}
                                        : std::initializer_list<double>{8., 6.}) +
              "," + radiusField + "," +
              fields(typeA ? std::initializer_list<double>{4., 2., 1., 2., 6., 1., 2.5, 1.5, 1.25}
                           : std::initializer_list<double>{3., 2., 3., 1., 1.5, .75}),
          scale);
      const auto model = LoadFromStep(source);
      complete(model);
      ASSERT_FALSE(model.indices.empty());
      watertight(model);
      EXPECT_NEAR(model.unitMetadata.metersPerUnit, 1. / scale, 1e-9);
      const double halfWidth = typeA ? 4. : 3.;
      expectProfileBounds(model, {3., -4. - halfWidth, 0.},
                          {11., -4. + halfWidth, 2.}, scale);

      const auto hasStation = [&](double x, double y) {
        const glm::vec3 point((7. - y) * scale, (-4. + x) * scale, 2. * scale);
        return std::ranges::any_of(model.vertices, [&](const auto &vertex) {
          return vertex.position == point;
        });
      };
      for (double sign : {-1., 1.}) {
        EXPECT_TRUE(hasStation(sign * halfWidth, -3.));
        EXPECT_TRUE(hasStation(sign * (typeA ? 1. : 1.5), typeA ? -1.5 : -2.5));
        EXPECT_TRUE(hasStation(sign * (typeA ? 2. : 3.), typeA ? 3. : 2.));
        if (typeA)
          EXPECT_TRUE(hasStation(sign * 3., -2.5));
        EXPECT_EQ(hasStation(sign * (typeA ? 2. : 3.), 4.), radius == 0.);
        if (radius > 0) {
          const glm::dvec2 center(3. + radius,
                                  -4. + sign * ((typeA ? 2. : 3.) - radius));
          EXPECT_TRUE(std::ranges::any_of(model.vertices, [&](const auto &vertex) {
            const auto radial = glm::dvec2(vertex.position) / scale - center;
            return std::abs(glm::length(radial) - radius) < 1e-5 &&
                   glm::dot(radial, glm::dvec2(-1., sign)) > 1.3 * radius;
          }));
        }
      }

      // Integrate the independent horizontal strips from the six station
      // heights. The crown cuts two squares minus two quarter disks.
      const double area = (typeA ? 8. + 3.5 + 4. + 7. + 3. + 4.
                                 : 6. + 2.25 + 10.5 + 4.5 + 12.) -
                          (2. - pi / 2.) * radius * radius;
      double capArea = 0.;
      for (size_t i = 0; i < model.indices.size(); i += 3) {
        const auto at = [&](size_t index) {
          return glm::dvec3(model.vertices.at(model.indices.at(index)).position) / scale;
        };
        const auto a = at(i), b = at(i + 1), c = at(i + 2);
        if (a.z == 2. && b.z == 2. && c.z == 2.)
          capArea += glm::cross(b - a, c - a).z / 2.;
      }
      EXPECT_NEAR(capArea, area, .004);
      EXPECT_NEAR(volume(model) / (scale * scale * scale), 2. * area, .008);
    }
}

TEST(IfcStructuralProfiles, InvalidCraneRailProfilesRejectAtomically) {
  struct Case {
    const char *type, *fields, *reason;
    const char *schema = "IFC2X3";
  };
  for (const auto &[type, fields, reason, schema] : std::array{
           Case{"A", ".AREA.,$,#4,8.,8.,$,4.,2.,1.,2.,9.,1.,2.5,1.5,$", "BaseWidth4"},
           Case{"A", ".AREA.,$,#4,8.,8.,$,4.,2.,1.,2.,6.,1.,2.5,3.,$", "BaseDepth3"},
           Case{"A", ".AREA.,$,#4,8.,8.,$,4.,2.,1.,2.,6.,1.,7.,1.5,$", "stations overlap"},
           Case{"A", ".AREA.,$,#4,8.,8.,1.1,4.,2.,1.,2.,6.,1.,2.5,1.5,$", "crown Radius"},
           Case{"A", ".AREA.,$,#4,8.,8.,$,4.,2.,1.,2.,6.,1.,2.5,$,$", "BaseDepth3"},
           Case{"F", ".AREA.,$,#4,8.,6.,-1.,3.,2.,3.,1.,1.5,$", "Radius"},
           Case{"F", ".AREA.,$,#4,8.,6.,$,3.,2.,3.,1.,1.5,-.75", "CentreOfGravityInY"},
           Case{"F", ".AREA.,$,$,8.,6.,$,3.,2.,3.,1.,1.5,$", "missing 2D placement"},
           Case{"F", ".CURVE.,$,#4,8.,6.,$,3.,2.,3.,1.,1.5,$", "AREA profile"},
           Case{"F", ".AREA.,$,#4,8.,6.,$,3.,2.,3.,1.,1.5,$", "IFC2X3 FILE_SCHEMA", "IFC4"}}) {
    SCOPED_TRACE(fields);
    const auto model = LoadFromStep(craneRailFixture(type, fields, 1., schema));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    EXPECT_TRUE(model.meshRanges.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    const auto &diagnostic = model.importReport.diagnostics.front();
    EXPECT_EQ(diagnostic.entityId, 10u);
    EXPECT_NE(diagnostic.reason.find(std::string("IFCCRANERAIL") + type +
                                     "SHAPEPROFILEDEF #5"), std::string::npos);
    EXPECT_NE(diagnostic.reason.find(reason), std::string::npos);
  }
}

TEST(IfcStructuralProfiles, RoundedAndHollowRectanglesPreserveIndependentCornerRadii) {
  const auto roundedArea = [](double width, double height, double radius) {
    return width * height - (4 - pi) * radius * radius;
  };
  for (bool hollow : {false, true}) {
    SCOPED_TRACE(hollow);
    const auto source = fixture(std::string(hollow
        ? "#5=IFCRECTANGLEHOLLOWPROFILEDEF(.AREA.,$,$,8.,6.,1.,0.25,1.25);"
        : "#5=IFCROUNDEDRECTANGLEPROFILEDEF(.AREA.,$,$,8.,6.,1.);") +
        "#10=IFCEXTRUDEDAREASOLID(#5,$,$,2.);");
    const auto model = LoadFromStep(source);
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    const double area = hollow ? roundedArea(8, 6, 1.25) - roundedArea(6, 4, .25)
                               : roundedArea(8, 6, 1);
    EXPECT_NEAR(volume(model), area * 2, .03);
    glm::vec3 minimum(std::numeric_limits<float>::infinity());
    glm::vec3 maximum(-minimum);
    for (const auto &vertex : model.vertices) {
      minimum = glm::min(minimum, vertex.position);
      maximum = glm::max(maximum, vertex.position);
    }
    EXPECT_EQ(minimum, glm::vec3(-4, -3, 0));
    EXPECT_EQ(maximum, glm::vec3(4, 3, 2));

    // Every side vertex belongs to one prescribed rounded rectangle. Using
    // the analytic signed distance checks straight/arc joins and both radii.
    const auto distance = [](glm::dvec2 p, glm::dvec2 halfExtent, double radius) {
      const auto q = glm::abs(p) - halfExtent + radius;
      return glm::length(glm::max(q, glm::dvec2(0))) +
             std::min(std::max(q.x, q.y), 0.) - radius;
    };
    size_t cavityFaces = 0;
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      const glm::dvec3 a = model.vertices.at(model.indices.at(i)).position;
      const glm::dvec3 b = model.vertices.at(model.indices.at(i + 1)).position;
      const glm::dvec3 c = model.vertices.at(model.indices.at(i + 2)).position;
      if (a.z == b.z && b.z == c.z) continue;
      const auto radius = hollow ? 1.25 : 1.;
      const bool cavity = hollow && std::abs(distance(glm::dvec2(a), {3, 2}, .25)) < 1e-5;
      for (const auto &point : {a, b, c}) {
        EXPECT_NEAR(distance(glm::dvec2(point), cavity ? glm::dvec2(3, 2) : glm::dvec2(4, 3),
                             cavity ? .25 : radius), 0, 1e-5);
      }
      const auto normal = glm::cross(b - a, c - a);
      const auto center = (a + b + c) / 3.;
      EXPECT_GT(glm::dot(glm::dvec2(normal), glm::dvec2(center)) * (cavity ? -1 : 1), 0);
      cavityFaces += cavity;
    }
    if (hollow) EXPECT_GT(cavityFaces, 0u);
    exportProfileFixture(hollow ? "area-hollow-rectangle" : "area-rounded-rectangle", source);
  }
}

TEST(IfcStructuralProfiles, EllipseTessellationRetainsPhysicalChordErrorWithPlacedMillimetreProfile) {
  const auto source = fixture(
      "#2=IFCCARTESIANPOINT((9000.,-7000.)); #3=IFCDIRECTION((0.,1.));"
      "#4=IFCAXIS2PLACEMENT2D(#2,#3); #5=IFCELLIPSEPROFILEDEF(.AREA.,$,#4,3000.,2000.);"
      "#7=IFCCARTESIANPOINT((0.,0.));"
      "#8=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#7,2.,0.5);"
      "#9=IFCDERIVEDPROFILEDEF(.AREA.,$,#5,#8,$);"
      "#11=IFCMIRROREDPROFILEDEF(.AREA.,$,#9,*,$);"
      "#10=IFCEXTRUDEDAREASOLID(#11,$,$,3000.);");
  auto mmSource = source;
  const auto metres = mmSource.find(".LENGTHUNIT.,$,.METRE.");
  ASSERT_NE(metres, std::string::npos);
  mmSource.replace(metres, std::string(".LENGTHUNIT.,$,.METRE.").size(),
                   ".LENGTHUNIT.,.MILLI.,.METRE.");
  const auto model = LoadFromStep(mmSource);
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model) * 1e-9, 18 * pi, .04);
  ASSERT_TRUE(model.unitMetadata.hasMetersPerUnit);
  EXPECT_FLOAT_EQ(model.unitMetadata.metersPerUnit, .001f);
  std::map<double, glm::dvec2> contour;
  glm::dvec3 minimum(std::numeric_limits<double>::infinity());
  glm::dvec3 maximum(-minimum);
  // The placed and derived ellipse has centre (-18,-3.5) and semiaxes
  // (4,1.5). Check each generated chord against the actual ellipse.
  const glm::dvec2 center(-18, -3.5);
  const glm::dvec2 axes(4, 1.5);
  for (const auto &vertex : model.vertices) {
    const glm::dvec3 p = glm::dvec3(vertex.position) / 1000.;
    minimum = glm::min(minimum, p);
    maximum = glm::max(maximum, p);
    const auto local = (glm::dvec2(p) - center) / axes;
    EXPECT_NEAR(glm::dot(local, local), 1, 2e-6);
    if (p.z == 0) contour[std::atan2(local.y, local.x)] = glm::dvec2(p);
  }
  EXPECT_EQ(minimum, glm::dvec3(-22, -5, 0));
  EXPECT_EQ(maximum, glm::dvec3(-14, -2, 3));
  ASSERT_GT(contour.size(), 16u);
  for (auto a = contour.begin(); a != contour.end(); ++a) {
    auto b = std::next(a);
    const bool seam = b == contour.end();
    if (seam) b = contour.begin();
    const double angle = (a->first + b->first + (seam ? 2 * pi : 0)) * .5;
    const auto exact = center + axes * glm::dvec2(std::cos(angle), std::sin(angle));
    const auto d = b->second - a->second;
    const auto nearest = a->second + d * std::clamp(glm::dot(exact - a->second, d) / glm::dot(d, d), 0., 1.);
    EXPECT_LE(glm::length(exact - nearest), .00102);
  }
  exportProfileFixture("area-derived-ellipse-mm", mmSource);
}

TEST(IfcStructuralProfiles, TrapeziumUsesSignedTopOffsetFromBottomLeft) {
  for (double offset : {-1., 5.}) {
    SCOPED_TRACE(offset);
    const auto source = fixture("#5=IFCTRAPEZIUMPROFILEDEF(.AREA.,$,$,4.,2.,3.," + number(offset) +
                                "); #10=IFCEXTRUDEDAREASOLID(#5,$,$,2.);");
    const auto model = LoadFromStep(source);
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model), 18, 1e-5);
    std::set<std::array<float, 2>> corners;
    for (const auto &vertex : model.vertices) {
      if (vertex.position.z == 0) corners.insert({vertex.position.x, vertex.position.y});
    }
    const auto topLeft = static_cast<float>(offset - 2);
    const std::set<std::array<float, 2>> expected{{{-2, -1.5f}, {2, -1.5f},
                                                     {topLeft, 1.5f}, {topLeft + 2, 1.5f}}};
    EXPECT_EQ(corners, expected);
    if (offset > 0) exportProfileFixture("area-trapezium", source);
  }
}

TEST(IfcStructuralProfiles, LinearCenterlineExtrusionsUseMiterCornersAndButtEnds) {
  for (bool segmented : {false, true}) {
    const std::string curves = segmented
        ? "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(4.,0.),(4.,3.)));"
          "#4=IFCINDEXEDPOLYCURVE(#2,(IFCLINEINDEX((1,2)),IFCLINEINDEX((2,3))),.F.);"
        : "#2=IFCCARTESIANPOINT((0.,0.)); #3=IFCCARTESIANPOINT((4.,0.));"
          "#6=IFCCARTESIANPOINT((4.,3.)); #4=IFCPOLYLINE((#2,#3,#6));";
    const auto source = fixture(curves + "#5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);"
                                         "#10=IFCEXTRUDEDAREASOLID(#5,$,$,2.);");
    const auto model = LoadFromStep(source);
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model), 14, 1e-5);
    std::set<std::array<float, 2>> corners;
    for (const auto &vertex : model.vertices) {
      if (vertex.position.z == 0) corners.insert({vertex.position.x, vertex.position.y});
    }
    const std::set<std::array<float, 2>> expected{{{0, -.5f}, {4.5f, -.5f}, {4.5f, 3},
                                                     {3.5f, 3}, {3.5f, .5f}, {0, .5f}}};
    EXPECT_EQ(corners, expected);
    if (segmented) exportProfileFixture("area-centerline", source);
  }
}

void expectCurvedCenterlineSector(const Model &model, bool tangentLine,
                                  glm::dvec2 scale = {1, 1},
                                  glm::dvec2 translation = {0, 0}) {
  watertight(model);
  constexpr double tolerance = 2e-6;
  const double startY = tangentLine ? -3. : 0.;
  const double capArea = (2 * pi - startY) * std::abs(scale.x * scale.y);
  EXPECT_NEAR(volume(model), 2 * capArea, .03);
  std::array<double, 2> area{};
  std::array<unsigned, 4> walls{};
  double maxChordError = 0.;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    std::array<glm::dvec3, 3> points;
    std::array<glm::dvec2, 3> base;
    for (size_t j = 0; j < points.size(); ++j) {
      points[j] = model.vertices.at(model.indices.at(i + j)).position;
      base[j] = (glm::dvec2(points[j]) - translation) / scale;
      EXPECT_GE(base[j].x, -tolerance);
      EXPECT_LE(base[j].x, 4.5 + tolerance);
      EXPECT_GE(base[j].y, startY - tolerance);
      EXPECT_LE(base[j].y, 4.5 + tolerance);
    }
    const auto n = glm::cross(points[1] - points[0], points[2] - points[0]);
    ASSERT_GT(glm::length(n), 0.);
    if (points[0].z == points[1].z && points[1].z == points[2].z) {
      const bool top = points[0].z > 0;
      EXPECT_EQ(points[0].z, top ? 2. : 0.);
      EXPECT_GT((top ? 1 : -1) * n.z, 0.);
      area[top] += .5 * glm::length(n);
      continue;
    }
    glm::dvec2 expected;
    if (std::ranges::all_of(base, [&](glm::dvec2 p) {
          return std::abs(p.y - startY) <= tolerance;
        })) {
      expected = {0, -1};
      ++walls[0];
    } else if (std::ranges::all_of(base, [](glm::dvec2 p) {
                 return std::abs(p.x) <= tolerance;
               })) {
      expected = {-1, 0};
      ++walls[1];
    } else {
      const auto centroid = (base[0] + base[1] + base[2]) / 3.;
      const bool line = tangentLine && centroid.y <= 0.;
      const bool outer = (line ? centroid.x : glm::length(centroid)) > 4.;
      const double radius = outer ? 4.5 : 3.5;
      const double sign = outer ? 1. : -1.;
      for (const auto p : base) {
        EXPECT_NEAR(line ? p.x : glm::length(p), radius, tolerance);
        EXPECT_TRUE(line ? p.y <= tolerance : p.y >= -tolerance);
      }
      if (line) {
        expected = {sign, 0};
      } else {
        expected = sign * glm::normalize(centroid);
        for (size_t j = 0; j < base.size(); ++j) {
          const auto k = (j + 1) % base.size();
          const auto a = glm::dvec2(points[j]), b = glm::dvec2(points[k]);
          const auto edge = b - a;
          const double lengthSquared = glm::dot(edge, edge);
          if (lengthSquared < 1e-12)
            continue;
          const auto exact =
              translation + scale * radius * glm::normalize(base[j] + base[k]);
          const double t =
              std::clamp(glm::dot(exact - a, edge) / lengthSquared, 0., 1.);
          maxChordError =
              std::max(maxChordError, glm::length(exact - a - t * edge));
        }
        ++walls[2 + outer];
      }
    }
    EXPECT_GT(glm::dot(glm::normalize(glm::dvec2(n)),
                       glm::normalize(expected / scale)),
              .995);
  }
  for (double cap : area)
    EXPECT_NEAR(cap, capArea, .015);
  for (unsigned count : walls)
    EXPECT_GT(count, 0u);
  EXPECT_LE(maxChordError, .00101);
}

TEST(IfcStructuralProfiles, CurvedCenterlineExtrusionsKeepExactAnnularSector) {
  constexpr std::string_view indexed =
      "#2=IFCCARTESIANPOINTLIST2D(((4.,0.),(2.4,3.2),(0.,4.)));"
      "#4=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((1,2,3))),.F.);";
  const std::string reversed =
      "#2=IFCCARTESIANPOINT((0.,0.)); #3=IFCAXIS2PLACEMENT2D(#2,$);"
      "#6=IFCCIRCLE(#3,4.);"
      "#4=IFCTRIMMEDCURVE(#6,(IFCPARAMETERVALUE(90.)),"
      "(IFCPARAMETERVALUE(0.)),.F.,.PARAMETER.);" +
      degrees();
  for (bool circleTrim : {false, true}) {
    SCOPED_TRACE(circleTrim);
    const auto source =
        fixture((circleTrim ? reversed : std::string(indexed)) +
                    "#5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);"
                    "#10=IFCEXTRUDEDAREASOLID(#5,$,$,2.);",
                "#10", "'Body','SweptSolid'");
    const auto model = LoadFromStep(source);
    EXPECT_NO_FATAL_FAILURE(complete(model));
    if (!model.indices.empty())
      expectCurvedCenterlineSector(model, false);
    if (!circleTrim)
      exportProfileFixture("area-centerline-curved", source);
  }
}

TEST(IfcStructuralProfiles, CurvedCenterlineExtrusionsKeepTangentLineAndArc) {
  constexpr std::string_view indexed =
      "#2=IFCCARTESIANPOINTLIST2D(((4.,-3.),(4.,0.),(2.4,3.2),(0.,4.)));"
      "#4=IFCINDEXEDPOLYCURVE(#2,(IFCLINEINDEX((1,2)),IFCARCINDEX((2,3,4))),.F."
      ");";
  const std::string composite =
      "#2=IFCCARTESIANPOINT((0.,0.)); #3=IFCAXIS2PLACEMENT2D(#2,$);"
      "#6=IFCCIRCLE(#3,4.); #7=IFCTRIMMEDCURVE(#6,(IFCPARAMETERVALUE(0.)),"
      "(IFCPARAMETERVALUE(90.)),.T.,.PARAMETER.);"
      "#8=IFCCARTESIANPOINT((4.,0.)); #9=IFCCARTESIANPOINT((4.,-3.));"
      "#12=IFCPOLYLINE((#8,#9));"
      "#13=IFCCOMPOSITECURVESEGMENT(.CONTSAMEGRADIENT.,.F.,#12);"
      "#14=IFCCOMPOSITECURVESEGMENT(.DISCONTINUOUS.,.T.,#7);"
      "#4=IFCCOMPOSITECURVE((#13,#14),.F.);" +
      degrees();
  for (bool useComposite : {false, true}) {
    for (bool transformed : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "composite=" << useComposite
                                        << " transformed=" << transformed);
      auto definitions = (useComposite ? composite : std::string(indexed)) +
                         "#5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);";
      if (transformed)
        definitions += "#60=IFCCARTESIANPOINT((1.,-2.));"
                       "#61=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,"
                       "#60,2.5,0.75);"
                       "#62=IFCDERIVEDPROFILEDEF(.AREA.,$,#5,#61,$);"
                       "#63=IFCMIRROREDPROFILEDEF(.AREA.,$,#62,*,$);";
      const auto source =
          fixture(definitions + "#10=IFCEXTRUDEDAREASOLID(" +
                      (transformed ? "#63" : "#5") + ",$,$,2.);",
                  "#10", "'Body','SweptSolid'");
      const auto model = LoadFromStep(source);
      EXPECT_NO_FATAL_FAILURE(complete(model));
      if (!model.indices.empty()) {
        const auto scale = transformed ? glm::dvec2(-2.5, .75) : glm::dvec2(1);
        const auto origin = transformed ? glm::dvec2(-1, -2) : glm::dvec2(0);
        expectCurvedCenterlineSector(model, true, scale, origin);
      }
      if (!transformed && useComposite)
        exportProfileFixture("area-centerline-tangent", source);
    }
  }
}

TEST(IfcStructuralProfiles, CurvedCenterlineExtrusionsJoinOppositeCurvatures) {
  // The first turn is counterclockwise about (0,0), the second clockwise
  // about (0,6). Both point along -X at their common endpoint (0,4).
  const auto source = fixture("#2=IFCCARTESIANPOINTLIST2D(((4.,0.),(2.4,3.2),("
                              "0.,4.),(-1.2,4.4),(-2.,6.)));"
                              "#4=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((1,2,3)),"
                              "IFCARCINDEX((3,4,5))),.F.);"
                              "#5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);"
                              "#10=IFCEXTRUDEDAREASOLID(#5,$,$,2.);",
                              "#10", "'Body','SweptSolid'");
  const auto model = LoadFromStep(source);
  ASSERT_NO_FATAL_FAILURE(complete(model));
  watertight(model);
  EXPECT_NEAR(volume(model), 6 * pi, .03);
  expectProfileBounds(model, {-2.5, 0, 0}, {4.5, 6, 2});
  for (const double radius : {3.5, 4.5})
    expectProfileArc(model, {0, 0}, radius, {1, 1});
  for (const double radius : {1.5, 2.5})
    expectProfileArc(model, {0, 6}, radius, {-1, -1});
  for (const float y : {3.5f, 4.5f})
    EXPECT_TRUE(std::ranges::any_of(model.vertices, [&](const auto &vertex) {
      return glm::length(vertex.position - glm::vec3(0, y, 0)) < 1e-6f;
    }));
}

TEST(IfcStructuralProfiles, CompositeRegionsPreserveMirrorsAndNestedIslands) {
  for (bool islandInsideVoid : {false, true}) {
    SCOPED_TRACE(islandInsideVoid);
    const std::string definitions =
        "#2=IFCCARTESIANPOINT((5.,0.)); #3=IFCAXIS2PLACEMENT2D(#2,$);"
        "#5=IFCRECTANGLEHOLLOWPROFILEDEF(.AREA.,$,#3,6.,4.,1.,$,$);"
        "#20=IFCMIRROREDPROFILEDEF(.AREA.,$,#5,*,$);"
        "#21=IFCRECTANGLEPROFILEDEF(.AREA.,$,#3,2.,1.);"
        "#22=IFCCOMPOSITEPROFILEDEF(.AREA.,$,(#5,#20" +
        std::string(islandInsideVoid ? ",#21" : "") + "),$);"
        "#10=IFCEXTRUDEDAREASOLID(#22,$,$,2.);";
    const auto source = fixture(definitions);
    const auto model = LoadFromStep(source);
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model), islandInsideVoid ? 68 : 64, 1e-5);
    EXPECT_NEAR(model.meshRanges.front().boundsCenter.x, 0, 1e-6);
    // Separate connected regions should never triangulate a cap across the
    // central gap. Island triangles stay entirely inside the right void.
    size_t cavityWalls = 0, islandWalls = 0;
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      const glm::dvec3 a = model.vertices.at(model.indices.at(i)).position;
      const glm::dvec3 b = model.vertices.at(model.indices.at(i + 1)).position;
      const glm::dvec3 c = model.vertices.at(model.indices.at(i + 2)).position;
      const auto low = glm::min(a, glm::min(b, c));
      const auto high = glm::max(a, glm::max(b, c));
      EXPECT_TRUE(high.x <= -2. || low.x >= 2.);
      if (a.z == b.z && b.z == c.z) continue;
      const auto normal = glm::cross(b - a, c - a);
      const auto center = (a + b + c) / 3.;
      const glm::dvec2 radial(center.x > 0 ? center.x - 5 : center.x + 5, center.y);
      const bool cavity = high.y <= 1. && low.y >= -1. &&
          (low.x >= 3. && high.x <= 7. || low.x >= -7. && high.x <= -3.);
      const bool island = low.x >= 4. && high.x <= 6. && low.y >= -.5 && high.y <= .5;
      EXPECT_GT(glm::dot(glm::dvec2(normal), radial) * (cavity && !island ? -1 : 1), 0);
      cavityWalls += cavity && !island;
      islandWalls += island;
    }
    EXPECT_EQ(cavityWalls, 16u);
    EXPECT_EQ(islandWalls, islandInsideVoid ? 8u : 0u);
    if (islandInsideVoid) exportProfileFixture("area-composite-void-island", source);
  }
}

TEST(IfcStructuralProfiles, MappedCompositeExtrusionsRetainWorldBoundsAndVolume) {
  auto source = fixture(
      "#2=IFCCARTESIANPOINT((5000.,0.)); #3=IFCAXIS2PLACEMENT2D(#2,$);"
      "#5=IFCRECTANGLEHOLLOWPROFILEDEF(.AREA.,$,#3,6000.,4000.,1000.,$,$);"
      "#20=IFCMIRROREDPROFILEDEF(.AREA.,$,#5,*,$);"
      "#22=IFCCOMPOSITEPROFILEDEF(.AREA.,$,(#5,#20),$);"
      "#10=IFCEXTRUDEDAREASOLID(#22,$,$,2000.);"
      "#31=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#10));"
      "#32=IFCREPRESENTATIONMAP($,#31);"
      "#33=IFCCARTESIANPOINT((12000.,5000.,3000.));"
      "#34=IFCDIRECTION((0.,1.,0.)); #35=IFCDIRECTION((-1.,0.,0.));"
      "#36=IFCDIRECTION((0.,0.,1.));"
      "#37=IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM(#34,#35,#33,2.,#36,0.5,3.);"
      "#38=IFCMAPPEDITEM(#32,#37);", "#38");
  const auto metres = source.find(".LENGTHUNIT.,$,.METRE.");
  ASSERT_NE(metres, std::string::npos);
  source.replace(metres, std::string(".LENGTHUNIT.,$,.METRE.").size(),
                 ".LENGTHUNIT.,.MILLI.,.METRE.");
  const auto model = LoadFromStep(source, .5f);
  complete(model);
  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  const auto &element = model.elements.front();
  glm::dvec3 minimum(std::numeric_limits<double>::infinity()), maximum(-minimum);
  double transformedVolume = 0;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    std::array<glm::dvec3, 3> points;
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto p = model.vertices.at(model.indices.at(i + corner)).position;
      points[corner] = glm::dvec3(glm::dmat4(element.transform) * glm::dvec4(p, 1));
      minimum = glm::min(minimum, points[corner]);
      maximum = glm::max(maximum, points[corner]);
    }
    transformedVolume += glm::dot(points[0], glm::cross(points[1], points[2])) / 6.;
  }
  // Source rotation sends x to +Y and y to -X; source axes map as
  // (X,Y,Z) -> (X,Z,-Y). The independent affine determinant is 3 / 8.
  EXPECT_LT(glm::length(minimum - glm::dvec3(5.5, 1.5, -10.5)), 1e-5);
  EXPECT_LT(glm::length(maximum - glm::dvec3(6.5, 4.5, 5.5)), 1e-5);
  EXPECT_NEAR(transformedVolume, 64 * 3. / 8, 1e-5);
  exportProfileFixture("area-composite-mapped-mm", source);
}

TEST(IfcStructuralProfiles, InvalidExtendedProfilesRetireEveryFragmentAndKeepFallback) {
  struct Case {
    const char *profileType;
    const char *definition;
  };
  const std::array cases{
      Case{"IFCCSHAPEPROFILEDEF",
           "#5=IFCCSHAPEPROFILEDEF(.AREA.,$,$,10.,6.,0.5,0.5,$);"},
      Case{"IFCCSHAPEPROFILEDEF",
           "#5=IFCCSHAPEPROFILEDEF(.AREA.,$,$,10.,6.,0.5,2.,2.6);"},
      Case{"IFCZSHAPEPROFILEDEF",
           "#5=IFCZSHAPEPROFILEDEF(.AREA.,$,$,10.,2.,2.,1.,$,$);"},
      Case{"IFCZSHAPEPROFILEDEF",
           "#5=IFCZSHAPEPROFILEDEF(.AREA.,$,$,10.,5.,2.,1.,0.6,1.1);"},
      Case{"IFCTSHAPEPROFILEDEF",
           "#5=IFCTSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,2.,1.,$,$,$,0.3,$);"},
      Case{"IFCTSHAPEPROFILEDEF",
           "#5=IFCTSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,2.,1.,0.5,1.1,0.3,$,$);"},
      Case{"IFCASYMMETRICISHAPEPROFILEDEF",
           "#5=IFCASYMMETRICISHAPEPROFILEDEF(.AREA.,$,$,9.,8.,1.,1.5,$,3.,6.,$,"
           "$,0.6435011087932844,$,-1.2587542052323633);"},
      Case{"IFCASYMMETRICISHAPEPROFILEDEF",
           "#5=IFCASYMMETRICISHAPEPROFILEDEF(.AREA.,$,$,9.,10.,2.,1.5,0.6,6.,$,"
           "$,2.4);"},
      Case{"IFCASYMMETRICISHAPEPROFILEDEF",
           "/* FILE_SCHEMA(('IFC2X3')); */"
           "#5=IFCASYMMETRICISHAPEPROFILEDEF(.AREA.,'FILE_SCHEMA((''IFC2X3''));"
           "',$,9.,10.,2.,1.5,0.6,6.,$,$,2.4);"},
      Case{"IFCELLIPSEPROFILEDEF", "#5=IFCELLIPSEPROFILEDEF(.AREA.,$,$,3.,0.);"},
      Case{"IFCTRAPEZIUMPROFILEDEF", "#5=IFCTRAPEZIUMPROFILEDEF(.AREA.,$,$,0.,3.,4.,1.);"},
      Case{"IFCROUNDEDRECTANGLEPROFILEDEF", "#5=IFCROUNDEDRECTANGLEPROFILEDEF(.AREA.,$,$,8.,6.,3.5);"},
      Case{"IFCRECTANGLEHOLLOWPROFILEDEF", "#5=IFCRECTANGLEHOLLOWPROFILEDEF(.AREA.,$,$,8.,6.,3.,$,$);"},
      Case{"IFCRECTANGLEHOLLOWPROFILEDEF", "#5=IFCRECTANGLEHOLLOWPROFILEDEF(.AREA.,$,$,8.,6.,1.,3.,1.25);"},
      Case{"IFCRECTANGLEHOLLOWPROFILEDEF", "#5=IFCRECTANGLEHOLLOWPROFILEDEF(.AREA.,$,$,8.,6.,0.25,0.,2.);"},
      Case{"IFCCENTERLINEPROFILEDEF",
           "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(4.,0.),(4.,3.),(0.,0.)));"
           "#4=IFCINDEXEDPOLYCURVE(#2,$,.F.); #5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);"},
      Case{"IFCCENTERLINEPROFILEDEF",
           "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(4.,0.),(0.2,0.1)));"
           "#4=IFCINDEXEDPOLYCURVE(#2,$,.F.); #5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);"},
      // The thickness equals the diameter and collapses the inner radius.
      Case{"IFCCENTERLINEPROFILEDEF",
           "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(2.,2.),(4.,0.)));"
           "#4=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((1,2,3))),.F.);"
           "#5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,4.);"},
      Case{"IFCCENTERLINEPROFILEDEF",
           "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(2.,0.),(4.,0.)));"
           "#4=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((1,2,3))),.F.);"
           "#5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);"},
      // The tangent joins, but the line and arc have different curvatures.
      Case{"IFCCENTERLINEPROFILEDEF",
           "#2=IFCCARTESIANPOINTLIST2D(((4.,-3.),(4.,0.),(2.4,3.2),(0.,4.)));"
           "#6=IFCINDEXEDPOLYCURVE(#2,(IFCLINEINDEX((1,2))),.F.);"
           "#7=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((2,3,4))),.F.);"
           "#8=IFCCOMPOSITECURVESEGMENT(.CONTSAMEGRADIENTSAMECURVATURE.,.T.,#6);"
           "#9=IFCCOMPOSITECURVESEGMENT(.DISCONTINUOUS.,.T.,#7);"
           "#4=IFCCOMPOSITECURVE((#8,#9),.F.);"
           "#5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);"},
      // This simple centerline's tangent tail crosses the first butt end of its stroke.
      Case{"IFCCENTERLINEPROFILEDEF",
           "#2=IFCCARTESIANPOINTLIST2D(((4.,0.),(-4.,0.),(3.84,-1.12),"
           "(4.4,0.8)));"
           "#4=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((1,2,3)),"
           "IFCLINEINDEX((3,4))),.F.);"
           "#5=IFCCENTERLINEPROFILEDEF(.AREA.,$,#4,1.);"}};
  constexpr std::string_view sweep = "#10=IFCEXTRUDEDAREASOLID(#5,$,$,2.);";
  constexpr std::string_view fallback =
      "#40=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(4.,0.,0.),(0.,5.,0.)));"
      "#41=IFCTRIANGULATEDFACESET(#40,$,.F.,((1,2,3)),$);";
  const auto validate = [&](std::string definitions, std::string_view type) {
    SCOPED_TRACE(definitions);
    definitions += sweep;
    const auto failed = LoadFromStep(fixture(definitions));
    EXPECT_EQ(failed.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(failed.vertices.empty());
    EXPECT_TRUE(failed.indices.empty());
    EXPECT_TRUE(failed.meshRanges.empty());
    EXPECT_TRUE(failed.elements.empty());
    definitions += fallback;
    const auto partial = LoadFromStep(fixture(definitions, "#10,#41"));
    EXPECT_EQ(partial.importReport.completeness, ImportCompleteness::Partial);
    EXPECT_EQ(partial.importReport.importedProductCount, 1u);
    EXPECT_EQ(partial.importReport.partialProductCount, 1u);
    ASSERT_EQ(partial.importReport.diagnostics.size(), 1u);
    const auto &diagnostic = partial.importReport.diagnostics.front();
    EXPECT_EQ(diagnostic.entityId, 10u);
    EXPECT_EQ(diagnostic.productId, 900003u);
    EXPECT_EQ(diagnostic.productGuid, "brep-profile");
    EXPECT_NE(diagnostic.reason.find(std::string(type) + " #5"), std::string::npos);
    EXPECT_EQ(diagnostic.reason.find("Invalid or unsupported shape data"), std::string::npos);
    ASSERT_EQ(partial.elements.size(), 1u);
    EXPECT_EQ(partial.elements.front().guid, "brep-profile");
    EXPECT_EQ(partial.meshRanges.size(), 1u);
    EXPECT_EQ(partial.indices.size(), 3u);
    EXPECT_EQ(partial.vertices.size(), 3u);
  };
  for (const auto &[type, definitions] : cases) validate(definitions, type);

  constexpr std::string_view first =
      "#20=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.);"
      "#5=IFCCOMPOSITEPROFILEDEF(.AREA.,$,(#20,#21),$);";
  for (const auto *second : {
           "#21=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,2.,2.);", // contained
           "#2=IFCCARTESIANPOINT((3.,0.)); #3=IFCAXIS2PLACEMENT2D(#2,$);"
           "#21=IFCRECTANGLEPROFILEDEF(.AREA.,$,#3,2.,2.);", // touching
           "#21=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,6.,2.);", // crossing
           "#21=IFCRECTANGLEPROFILEDEF(.CURVE.,$,$,2.,2.);", // mixed
           "#21=IFCCOMPOSITEPROFILEDEF(.AREA.,$,(#5,#20),$);"}) { // recursive
    validate(std::string(first) + second, "IFCCOMPOSITEPROFILEDEF");
  }
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
                   const std::string &curve, bool sameSense = true) {
    for (const auto &p : {a, b})
      if (!vertices.contains(p))
        vertices[p] = add("IFCVERTEXPOINT(" + p + ")");
    return add("IFCEDGECURVE(" + vertices[a] + "," + vertices[b] + "," + curve +
               (sameSense ? ",.T.)" : ",.F.)"));
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

TEST(IfcCurvedBrep, PeriodicBandCutPreservesAKnotWithOneSidedNormals) {
  using container::geometry::ifc::detail::CurvedFaceChartLoop;
  using container::geometry::ifc::detail::meshIfcCurvedFace;
  using container::geometry::ifc::detail::ParametricSurface;
  constexpr double crease = .73, height = 2., slope = .3, period = 2 * pi;
  const auto radius = [=](double v) {
    return 1 + slope * std::abs(v - crease);
  };
  const auto point = [=](double u, double v) {
    return glm::dvec3(radius(v) * std::cos(u), radius(v) * std::sin(u), v);
  };
  ParametricSurface surface;
  surface.periods.x = period;
  surface.uDomain = {{0., period}};
  surface.vDomain = {{0., height}};
  surface.meshV = {crease};
  surface.creaseV = {crease};
  surface.spline = true;
  surface.point = point;
  surface.derivatives = [=](double u, double v) {
    const double c = std::cos(u), s = std::sin(u);
    const double dr = v < crease ? -slope : slope;
    return std::pair(glm::dvec3(-radius(v) * s, radius(v) * c, 0.),
                     glm::dvec3(dr * c, dr * s, 1.));
  };

  for (bool zipper : {false, true}) {
    SCOPED_TRACE(zipper ? "unequal offset rings" : "matching rings");
    constexpr double bottomStart = .173;
    const double topStart = zipper ? -.271 : bottomStart;
    std::vector<std::vector<glm::vec3>> loops;
    std::vector<CurvedFaceChartLoop> charts;
    const auto addRing = [&](unsigned count, double start, double z,
                             double direction) {
      auto &samples = loops.emplace_back();
      auto &chart = charts.emplace_back();
      chart.closure = {start + direction * period, z};
      for (unsigned i = 0; i < count; ++i) {
        const double u = start + direction * period * i / count;
        samples.emplace_back(point(u, z));
        chart.vertices.emplace_back(u, z);
      }
    };
    addRing(128, bottomStart, 0., 1.);
    addRing(zipper ? 160 : 128, topStart, height, -1.);

    std::string error;
    const auto mesh =
        meshIfcCurvedFace(surface, loops, false, true, 1., error, {}, charts);
    if (!mesh) {
      ADD_FAILURE() << error;
      continue;
    }
    ASSERT_FALSE(mesh->empty());

    // The cut connects the bottom start to the closest angular sample of the
    // upper ring. Its two virtual copies must produce identical float edges.
    const auto angularDifference = [=](glm::dvec2 uv) {
      return std::remainder(uv.x - bottomStart, period);
    };
    const auto topAnchor =
        std::ranges::min_element(charts[1].vertices, {}, [&](glm::dvec2 uv) {
          return std::abs(angularDifference(uv));
        });
    const auto onCut = [&](glm::vec3 p) {
      const double u =
          bottomStart + angularDifference(*topAnchor) * p.z / height;
      return glm::length(glm::dvec3(p) - point(u, p.z)) < 1e-6;
    };
    using Key = std::array<float, 3>;
    using Edge = std::pair<Key, Key>;
    const auto key = [](glm::vec3 p) { return Key{p.x, p.y, p.z}; };
    std::map<Edge, std::pair<unsigned, int>> uses;
    std::set<Edge> cutEdges;
    std::set<Key> cutAtCrease;
    bool cutNormalBelow = false, cutNormalAbove = false;
    double area = 0;
    const float knot = static_cast<float>(crease);
    for (const auto &triangle : *mesh) {
      const auto &p = triangle.positions;
      const auto [lo, hi] = std::ranges::minmax_element(p, {}, &glm::vec3::z);
      EXPECT_FALSE(lo->z < knot && hi->z > knot);
      area += glm::length(glm::cross(glm::dvec3(p[1]) - glm::dvec3(p[0]),
                                     glm::dvec3(p[2]) - glm::dvec3(p[0]))) /
              2.;
      for (size_t i = 0; i < p.size(); ++i) {
        const auto next = p[(i + 1) % p.size()];
        const auto a = key(p[i]), b = key(next);
        ASSERT_NE(a, b);
        const Edge edge = std::minmax(a, b);
        auto &use = uses[edge];
        ++use.first;
        use.second += a < b ? 1 : -1;
        if (onCut(p[i]) && onCut(next))
          cutEdges.emplace(edge);
        if (p[i].z != knot)
          continue;
        const bool below = lo->z < knot;
        const auto normal =
            glm::normalize(glm::dvec3(p[i].x, p[i].y, below ? slope : -slope));
        EXPECT_GT(glm::dot(glm::dvec3(triangle.normals[i]), normal), .99999);
        if (onCut(p[i])) {
          cutAtCrease.insert(a);
          cutNormalBelow |= below;
          cutNormalAbove |= !below;
        }
      }
    }
    EXPECT_EQ(cutAtCrease.size(), 1u);
    EXPECT_TRUE(cutNormalBelow);
    EXPECT_TRUE(cutNormalAbove);
    EXPECT_GE(cutEdges.size(), 2u);
    for (const auto &edge : cutEdges) {
      EXPECT_EQ(uses[edge].first, 2u);
      EXPECT_EQ(uses[edge].second, 0);
    }
    for (const auto &loop : loops)
      for (size_t i = 0; i < loop.size(); ++i) {
        const auto a = key(loop[i]), b = key(loop[(i + 1) % loop.size()]);
        const auto found = uses.find(std::minmax(a, b));
        ASSERT_NE(found, uses.end());
        EXPECT_EQ(found->second.first, 1u);
        EXPECT_EQ(found->second.second, a < b ? 1 : -1);
      }
    for (const auto &[_, use] : uses) {
      EXPECT_LE(use.first, 2u);
      if (use.first == 2)
        EXPECT_EQ(use.second, 0);
    }
    const auto singleUse = [](const auto &entry) {
      return entry.second.first == 1;
    };
    EXPECT_EQ(std::ranges::count_if(uses, singleUse),
              loops[0].size() + loops[1].size());
    const double integratedRadius =
        height +
        slope * (crease * crease + (height - crease) * (height - crease)) / 2.;
    EXPECT_NEAR(area, period * integratedRadius * std::hypot(1., slope), .02);
  }

  // A real shared side is immutable even when it crosses the same knot.
  const CurvedFaceChartLoop patch{
      {{.2, 0.}, {.24, 0.}, {.24, height}, {.2, height}}, {.2, 0.}};
  std::vector<glm::vec3> boundary;
  for (const auto &uv : patch.vertices)
    boundary.emplace_back(point(uv.x, uv.y));
  std::string error;
  EXPECT_FALSE(meshIfcCurvedFace(surface, {boundary}, true, true, 1., error, {},
                                 std::span(&patch, 1)));
  EXPECT_NE(error.find("Shared edge must have a sample"), std::string::npos)
      << error;
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

struct SphericalMeridians {
  std::string north, south;
  double radius;

  SphericalMeridians(CurvedBuilder &step, double r)
      : north(step.point({0, 0, r})), south(step.point({0, 0, -r})), radius(r) {
  }

  std::array<CurvedBuilder::Use, 2> arc(CurvedBuilder &step,
                                        glm::dvec3 radial) const {
    // The 3D circle's local X points toward the equator and its local Y
    // toward the north pole. These are two separately authored EDGE_CURVEs.
    const auto axis = glm::cross(radial, glm::dvec3(0, 0, 1));
    const auto basis =
        step.add("IFCCIRCLE(" + step.placement({}, axis, radial) + "," +
                 number(radius) + ")");
    const auto equator = step.point(radial * radius);
    return {{{step.edge(equator, north, basis), false},
             {step.edge(south, equator, basis), false}}};
  }
};

std::string explicitSphericalShell(CurvedBuilder &step, double radius,
                                   bool outward) {
  const SphericalMeridians meridians(step, radius);
  const auto east = meridians.arc(step, {1, 0, 0});
  const auto west = meridians.arc(step, {-1, 0, 0});
  const auto surface =
      step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + "," +
               number(radius) + ")");
  const auto start = step.faces.size();
  for (bool positive : {false, true}) {
    const auto &down = positive ? west : east;
    const auto &up = positive ? east : west;
    std::vector<CurvedBuilder::Use> edges{down[0],
                                          down[1],
                                          {up[1].first, !up[1].second},
                                          {up[0].first, !up[0].second}};
    if (!outward) {
      std::ranges::reverse(edges);
      for (auto &use : edges)
        use.second = !use.second;
    }
    // Use equivalent opposing authored bound senses on the two faces.
    step.face({step.bound(edges, true, !positive)}, surface, outward);
  }
  return step.add("IFCCLOSEDSHELL((" + step.faces[start] + "," +
                  step.faces[start + 1] + "))");
}

TEST(IfcCurvedBrep, SphericalEdgeLoopsJoinTwoHemispheresAtBothPoles) {
  CurvedBuilder step;
  constexpr double radius = 2.;
  const auto shell = explicitSphericalShell(step, radius, true);
  const auto brep = step.add("IFCADVANCEDBREP(" + shell + ")");
  const auto source = fixture(step.definitions, brep);
  const auto model = LoadFromStep(source);
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 32 * pi / 3, .06);
  std::array<bool, 2> poles{};
  for (const auto &vertex : model.vertices) {
    const auto p = glm::dvec3(vertex.position);
    EXPECT_NEAR(glm::length(p), radius, .0011);
    EXPECT_GT(glm::dot(p / radius, glm::dvec3(vertex.normal)), .99999);
    poles[0] |= p == glm::dvec3(0, 0, -radius);
    poles[1] |= p == glm::dvec3(0, 0, radius);
  }
  EXPECT_TRUE(poles[0] && poles[1]);
  exportCurvedFixture("sphere-edge-loop-hemispheres", source);
}

TEST(IfcCurvedBrep, SphericalEdgeLoopInsertsInteriorPolesOnSharedMeridian) {
  for (bool rational : {false, true}) {
    SCOPED_TRACE(rational);
    CurvedBuilder step;
    const double angle = .193;
    const glm::dvec3 start(2 * std::cos(angle), 0, 2 * std::sin(angle));
    const auto seam = step.point(start);
    std::string meridian;
    if (rational) {
      // Four exact rational quadratic spans with their knots rotated away
      // from the poles. Both poles lie strictly inside source spans.
      const glm::dvec3 tangent(start.z, 0, -start.x);
      const std::vector controls{seam,
                                 step.point(start + tangent),
                                 step.point(tangent),
                                 step.point(-start + tangent),
                                 step.point(-start),
                                 step.point(-start - tangent),
                                 step.point(-tangent),
                                 step.point(start - tangent),
                                 seam};
      const auto weight = number(1 / std::sqrt(2.));
      meridian = step.add("IFCRATIONALBSPLINECURVEWITHKNOTS(2,(" +
                          step.refs(controls) +
                          "),.UNSPECIFIED.,.T.,.F.,"
                          "(3,2,2,2,3),(0.,1.,2.,3.,4.),.UNSPECIFIED.,(1.," +
                          weight + ",1.," + weight + ",1.," + weight + ",1.," +
                          weight + ",1.))");
    } else {
      const auto equator = step.placement({0, 0, 0}, {0, 1, 0});
      meridian = step.add("IFCCIRCLE(" + equator + ",2.)");
    }
    const auto boundary = step.edge(seam, seam, meridian);
    const auto surface =
        step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + ",2.)");
    step.face({step.bound({{boundary, true}})}, surface);
    step.face({step.bound({{boundary, false}})}, surface);
    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model), 32 * pi / 3, .06);
    std::array<bool, 2> poles{};
    for (const auto &vertex : model.vertices) {
      poles[0] |= vertex.position == glm::vec3(0, 0, -2);
      poles[1] |= vertex.position == glm::vec3(0, 0, 2);
    }
    EXPECT_TRUE(poles[0] && poles[1]);
  }
}

struct MeridianSeamEncoding {
  double otherLongitude = 360.;
  double otherEndOffset = 0.;
  bool sameSense = true;
};

std::string polarMeridianEdge(CurvedBuilder &step, const std::string &surface,
                              const std::string &curve,
                              const std::string &first, const std::string &last,
                              double sourceFirst, double sourceLast,
                              MeridianSeamEncoding encoding) {
  const auto pcurve = [&](double u, double lastV) {
    const auto uv = [&](double v) {
      return step.add("IFCCARTESIANPOINT((" + number(u) + "," + number(v) +
                      "))");
    };
    const auto a = uv(sourceFirst), b = uv(lastV);
    const auto line = step.add("IFCPOLYLINE((" + a + "," + b + "))");
    return step.add("IFCPCURVE(" + surface + "," + line + ")");
  };
  const auto left = pcurve(0., sourceLast);
  const auto right =
      pcurve(encoding.otherLongitude, sourceLast + encoding.otherEndOffset);
  const auto seam = step.add("IFCSEAMCURVE(" + curve + ",(" + left + "," +
                             right + "),.CURVE3D.)");
  return step.edge(first, last, seam, encoding.sameSense);
}

std::string sphericalMeridianSeam(CurvedBuilder &step,
                                  const std::string &surface,
                                  const std::string &first,
                                  const std::string &last, double fromLatitude,
                                  double toLatitude,
                                  MeridianSeamEncoding encoding = {}) {
  const auto frame = step.placement({0, 0, 0}, {0, -1, 0});
  const auto circle = step.add("IFCCIRCLE(" + frame + ",2.)");
  const double from = encoding.sameSense ? fromLatitude : toLatitude;
  const double to = encoding.sameSense ? toLatitude : fromLatitude;
  const auto curve =
      step.add("IFCTRIMMEDCURVE(" + circle + ",(IFCPARAMETERVALUE(" +
               number(from) + ")),(IFCPARAMETERVALUE(" + number(to) + "))," +
               (to > from ? ".T.,.PARAMETER.)" : ".F.,.PARAMETER.)"));
  return polarMeridianEdge(step, surface, curve, first, last, from, to,
                           encoding);
}

TEST(IfcCurvedBrep, SphericalEdgeLoopAcceptsTheSameAuthoredMeridianSeamTwice) {
  CurvedBuilder step;
  const auto north = step.point({0, 0, 2}), south = step.point({0, 0, -2});
  const auto surface =
      step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + ",2.)");
  const auto edge =
      sphericalMeridianSeam(step, surface, south, north, -90., 90.);
  step.face({step.bound({{edge, true}, {edge, false}})}, surface);
  const auto brep = step.finish();
  const auto source = fixture(step.definitions + degrees(), brep);
  const auto model = LoadFromStep(source);
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 32 * pi / 3, .06);
  std::set<float> meridianHeights;
  for (const auto &vertex : model.vertices) {
    const auto p = glm::dvec3(vertex.position);
    EXPECT_NEAR(glm::length(p), 2., .0011);
    EXPECT_GT(glm::dot(p / 2., glm::dvec3(vertex.normal)), .99999);
    if (p.x > 1.8 && p.y == 0)
      meridianHeights.insert(float(p.z));
  }
  ASSERT_GE(meridianHeights.size(), 12u);
  EXPECT_LT(*meridianHeights.begin(), -.25f);
  EXPECT_GT(*meridianHeights.rbegin(), .25f);
  exportCurvedFixture("sphere-edge-loop-explicit-seam", source);
}

TEST(IfcCurvedBrep, SphericalEdgeLoopKeepsTrimmedMeridianSeamAndRimWithDisk) {
  for (unsigned variant : {0u, 1u, 2u}) {
    SCOPED_TRACE(variant);
    const bool reverseBound = variant == 1, invalid = variant == 2;
    CurvedBuilder step;
    constexpr double radius = 2., z = 1.;
    const auto north = step.point({0, 0, radius});
    const auto rim = step.circle(std::sqrt(radius * radius - z * z), z);
    const auto surface =
        step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + ",2.)");
    const auto seam =
        sphericalMeridianSeam(step, surface, rim.point, north, 30., 90.,
                              {.otherLongitude = invalid ? 180. : 360.});
    step.face({step.bound({{seam, false}, {rim.edge, true}, {seam, true}}, true,
                          !reverseBound)},
              surface);
    step.plane({{rim.edge, false}}, {0, 0, z}, {0, 0, -1});
    const auto brep = step.finish();
    const auto model =
        LoadFromStep(fixture(step.definitions + degrees(), brep));
    if (invalid) {
      EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
      EXPECT_TRUE(model.vertices.empty());
      EXPECT_TRUE(model.indices.empty());
      ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
      EXPECT_NE(model.importReport.diagnostics.front().reason.find("seam"),
                std::string::npos)
          << model.importReport.diagnostics.front().reason;
      continue;
    }
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model), 5 * pi / 3, .015);
    std::set<float> seamHeights;
    bool disk = false;
    for (const auto &vertex : model.vertices) {
      const auto p = vertex.position;
      if (p.y == 0.f && p.x > 0.f && p.z > 1.f && p.z < 2.f)
        seamHeights.insert(p.z);
      disk |= p.z == 1.f && vertex.normal.z < -.99999f;
    }
    EXPECT_GE(seamHeights.size(), 16u);
    EXPECT_TRUE(disk);
  }
}

double sphericalCapVolume(double plane) {
  constexpr double radius = 2.;
  const double height = radius - plane;
  return pi * height * height * (radius - height / 3.);
}

std::string sphericalRimAndDisk(CurvedBuilder &step, glm::dvec3 axis,
                                double plane) {
  constexpr double radius = 2.;
  const double ringRadius = std::sqrt(radius * radius - plane * plane);
  const glm::dvec3 tangent =
      axis.y == 0. ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
  const auto origin = plane * axis;
  const auto placement = step.placement(origin, axis, tangent);
  const auto circle =
      step.add("IFCCIRCLE(" + placement + "," + number(ringRadius) + ")");
  const auto start = step.point(origin + ringRadius * tangent);
  const auto ring = step.edge(start, start, circle);
  step.plane({{ring, true}}, origin, axis);
  return step.bound({{ring, false}}, false);
}

void expectSphericalRimAndDisk(const Model &model, glm::dvec3 axis,
                               double plane) {
  using Point = std::array<float, 3>;
  using Edge = std::pair<Point, Point>;
  std::map<Edge, std::array<int, 2>> edges;
  for (const auto &vertex : model.vertices)
    EXPECT_LE(glm::dot(glm::dvec3(vertex.position), axis), plane + 1e-6);
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    std::array<Point, 3> points;
    std::array<bool, 3> onPlane;
    bool disk = true;
    for (size_t j = 0; j < points.size(); ++j) {
      const auto &vertex = model.vertices.at(model.indices.at(i + j));
      const auto p = vertex.position;
      points[j] = {p.x, p.y, p.z};
      onPlane[j] = std::abs(glm::dot(glm::dvec3(p), axis) - plane) < 1e-6;
      disk &= onPlane[j] && glm::dot(glm::dvec3(vertex.normal), axis) > .99999;
    }
    for (size_t j = 0; j < points.size(); ++j) {
      const auto next = (j + 1) % points.size();
      if (!onPlane[j] || !onPlane[next])
        continue;
      const auto &a = points[j], &b = points[next];
      const Edge edge = std::minmax(a, b);
      edges[edge][disk] += a < b ? 1 : -1;
    }
  }
  size_t mated = 0;
  for (const auto &[_, winding] : edges) {
    if (winding[0] == 0 && winding[1] == 0)
      continue;
    EXPECT_EQ(std::abs(winding[1]), 1);
    EXPECT_EQ(winding[0], -winding[1]);
    ++mated;
  }
  EXPECT_GT(mated, 32u);
}

TEST(IfcCurvedBrep, SphericalSingleEdgeBoundRetainsItsSharedDisk) {
  struct Trim {
    glm::dvec3 axis;
    double plane;
  };
  constexpr std::array trims{Trim{{0, 0, 1}, -1.}, Trim{{1, 0, 0}, 1.}};
  for (const auto &[axis, plane] : trims) {
    SCOPED_TRACE(plane);
    CurvedBuilder step;
    const auto sphere = step.add("IFCSPHERICALSURFACE(" +
                                 step.placement({0, 0, 0}) + ",2.)");
    step.face({sphericalRimAndDisk(step, axis, plane)}, sphere);
    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    ASSERT_NO_FATAL_FAILURE(complete(model));
    watertight(model);
    EXPECT_NEAR(volume(model), sphericalCapVolume(-plane), .03);
    expectSphericalRimAndDisk(model, axis, plane);
    for (const auto &vertex : model.vertices) {
      const glm::dvec3 p(vertex.position), normal(vertex.normal);
      if (glm::dot(normal, axis) > .99999 &&
          std::abs(glm::dot(p, axis) - plane) < 1e-6)
        continue;
      EXPECT_NEAR(glm::length(p), 2., .0011);
      EXPECT_GT(glm::dot(p / 2., normal), .99999);
    }
  }
}

std::string sphericalFullSeamWithHole(glm::dvec3 axis, double plane) {
  CurvedBuilder step;
  const auto north = step.point({0, 0, 2}), south = step.point({0, 0, -2});
  const auto surface =
      step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + ",2.)");
  const auto seam =
      sphericalMeridianSeam(step, surface, south, north, -90., 90.);
  const auto hole = sphericalRimAndDisk(step, axis, plane);
  step.face({step.bound({{seam, true}, {seam, false}}), hole}, surface);
  const auto brep = step.finish();
  return fixture(step.definitions + degrees(), brep);
}

TEST(IfcCurvedBrep, SphericalFullMeridianSeamRetainsAnOffCutHoleAndDisk) {
  constexpr double plane = 1.6;
  const glm::dvec3 axis(0, 1, 0);
  const auto model = LoadFromStep(sphericalFullSeamWithHole(axis, plane));
  ASSERT_NO_FATAL_FAILURE(complete(model));
  watertight(model);
  EXPECT_NEAR(volume(model), 32 * pi / 3 - sphericalCapVolume(plane), .06);
  expectSphericalRimAndDisk(model, axis, plane);
}

TEST(IfcCurvedBrep, SphericalFullMeridianSeamMovesGeneratedCutAroundHole) {
  constexpr double plane = 1.6;
  const glm::dvec3 axis(-1, 0, 0);
  const auto source = sphericalFullSeamWithHole(axis, plane);
  exportCurvedFixture("sphere-explicit-seam-hole-across-generated-cut", source);
  const auto model = LoadFromStep(source);
  ASSERT_NO_FATAL_FAILURE(complete(model));
  watertight(model);
  EXPECT_NEAR(volume(model), 32 * pi / 3 - sphericalCapVolume(plane), .06);
  expectSphericalRimAndDisk(model, axis, plane);

  std::set<float> authoredHeights;
  for (const auto &vertex : model.vertices) {
    const auto p = vertex.position;
    if (p.x > 1.8f && p.y == 0.f)
      authoredHeights.insert(p.z);
  }
  ASSERT_GE(authoredHeights.size(), 12u);
  EXPECT_LT(*authoredHeights.begin(), -.25f);
  EXPECT_GT(*authoredHeights.rbegin(), .25f);
}

TEST(IfcCurvedBrep, SphericalTrimmedMeridianSeamMovesGeneratedCutAroundHoles) {
  CurvedBuilder step;
  constexpr double radius = 2., rimHeight = 1., lowerPlane = 1.94;
  const auto north = step.point({0, 0, radius});
  const auto rim = step.circle(
      std::sqrt(radius * radius - rimHeight * rimHeight), rimHeight);
  const auto surface =
      step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + ",2.)");
  const auto seam =
      sphericalMeridianSeam(step, surface, rim.point, north, 30., 90.);
  // Both cuts cross longitude 180 degrees in disjoint polar intervals and
  // avoid the authored longitude zero, the pole, and the lower rim.
  const glm::dvec3 lowerAxis(-.6, 0, .8);
  const double polar = pi / 15, upperPlane = radius * std::cos(pi / 30);
  const glm::dvec3 upperAxis(-std::sin(polar), 0, std::cos(polar));
  const auto lowerHole = sphericalRimAndDisk(step, lowerAxis, lowerPlane);
  const auto upperHole = sphericalRimAndDisk(step, upperAxis, upperPlane);
  step.face({step.bound({{seam, false}, {rim.edge, true}, {seam, true}}),
             lowerHole, upperHole},
            surface);
  step.plane({{rim.edge, false}}, {0, 0, rimHeight}, {0, 0, -1});
  const auto brep = step.finish();
  const auto source = fixture(step.definitions + degrees(), brep);
  exportCurvedFixture("sphere-trimmed-seam-holes-across-generated-cut", source);
  const auto model = LoadFromStep(source);
  ASSERT_NO_FATAL_FAILURE(complete(model));
  watertight(model);
  EXPECT_NEAR(volume(model),
              sphericalCapVolume(rimHeight) - sphericalCapVolume(lowerPlane) -
                  sphericalCapVolume(upperPlane),
              .015);
  expectSphericalRimAndDisk(model, lowerAxis, lowerPlane);
  expectSphericalRimAndDisk(model, upperAxis, upperPlane);
  std::set<float> authoredHeights;
  for (const auto &vertex : model.vertices) {
    const auto p = vertex.position;
    EXPECT_GE(p.z, static_cast<float>(rimHeight) - 1e-6f);
    if (p.x > 0.f && p.y == 0.f && p.z > rimHeight && p.z < radius)
      authoredHeights.insert(p.z);
  }
  EXPECT_GE(authoredHeights.size(), 16u);
}

TEST(IfcCurvedBrep, SphericalFullMeridianSeamRejectsAContactingHole) {
  const auto source = sphericalFullSeamWithHole({1, 0, 0}, 1.6);
  const auto model = LoadFromStep(source);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(model.elements.empty());
  EXPECT_TRUE(model.vertices.empty());
  EXPECT_TRUE(model.indices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &reason = model.importReport.diagnostics.front().reason;
  EXPECT_NE(reason.find("seam"), std::string::npos) << reason;
  EXPECT_NE(reason.find("hole"), std::string::npos) << reason;
}

void expectPolarMeridianJoin(const Model &model, glm::vec3 join,
                             const auto &onMeridian, unsigned orderAxis) {
  // The exact authored join must have a consecutive edge on each side.
  // Each edge must be used twice and in opposite directions across the seam.
  std::map<std::array<float, 3>, std::pair<unsigned, int>> neighbors;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    for (size_t j = 0; j < 3; ++j) {
      const auto a = model.vertices.at(model.indices.at(i + j)).position;
      const auto b =
          model.vertices.at(model.indices.at(i + (j + 1) % 3)).position;
      const bool outgoing = a == join && onMeridian(b);
      const bool incoming = b == join && onMeridian(a);
      if (outgoing == incoming)
        continue;
      const auto p = outgoing ? b : a;
      auto &[uses, winding] = neighbors[{p.x, p.y, p.z}];
      ++uses;
      winding += outgoing ? 1 : -1;
    }
  }
  ASSERT_EQ(neighbors.size(), 2u);
  unsigned prior = 0;
  for (const auto &[point, incidence] : neighbors) {
    EXPECT_EQ(incidence, (std::pair{2u, 0}));
    EXPECT_NE(point[orderAxis], join[orderAxis]);
    prior += point[orderAxis] < join[orderAxis];
  }
  EXPECT_EQ(prior, 1u);
}

glm::dvec3 sphereMeridianPoint(double latitude) {
  const double radians = latitude * pi / 180.;
  return {2. * std::cos(radians), 0., 2. * std::sin(radians)};
}

enum class SplitMeridianError { None, WrongPcurve, InteriorRim };

std::string splitSphericalMeridian(
    bool trimmed, double splitLatitude, bool reversed,
    SplitMeridianError defect = SplitMeridianError::None) {
  CurvedBuilder step;
  const auto north = step.point({0, 0, 2});
  const auto rim = trimmed ? step.circle(std::sqrt(3.), 1.)
                           : CurvedBuilder::Circle{step.point({0, 0, -2}), {}};
  const auto split = step.point(sphereMeridianPoint(splitLatitude));
  const auto sphere =
      step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + ",2.)");
  const auto lower = sphericalMeridianSeam(step, sphere, rim.point, split,
                                           trimmed ? 30. : -90., splitLatitude);
  const auto upper = sphericalMeridianSeam(
      step, sphere, split, north, splitLatitude, 90.,
      {.otherEndOffset = defect == SplitMeridianError::WrongPcurve ? -.75 : 0.,
       .sameSense = !reversed});
  std::vector<CurvedBuilder::Use> uses;
  if (trimmed) {
    uses = {{upper, false},
            {lower, false},
            {rim.edge, true},
            {lower, true},
            {upper, true}};
  } else
    uses = {{lower, true}, {upper, true}, {upper, false}, {lower, false}};
  if (defect == SplitMeridianError::InteriorRim) {
    const auto point = sphereMeridianPoint(splitLatitude);
    const auto frame = step.placement({0, 0, point.z});
    const auto circle = step.add("IFCCIRCLE(" + frame + "," + number(point.x) + ")");
    const auto extra = step.edge(split, split, circle);
    uses.insert(uses.begin() + 1, {extra, true});
    step.plane({{extra, false}}, {0, 0, point.z}, {0, 0, -1});
  }
  if (reversed)
    std::rotate(uses.begin(), uses.begin() + 1, uses.end());
  step.face({step.bound(std::move(uses), true, !reversed)}, sphere);
  if (trimmed)
    step.plane({{rim.edge, false}}, {0, 0, 1}, {0, 0, -1});
  const auto brep = step.finish();
  return fixture(step.definitions + degrees(), brep);
}

TEST(IfcCurvedBrep, SplitSphericalMeridianSeamsPreserveAuthoredJoin) {
  for (const bool trimmed : {false, true}) {
    const double latitude = trimmed ? 57. : 17.;
    for (const bool reversed : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "trimmed=" << trimmed << " reversed=" << reversed);
      const auto source = splitSphericalMeridian(trimmed, latitude, reversed);
      if (!reversed)
        exportCurvedFixture(trimmed ? "sphere-trimmed-split-seam"
                                    : "sphere-full-split-seam",
                            source);
      const auto model = LoadFromStep(source);
      EXPECT_NO_FATAL_FAILURE(complete(model));
      if (model.indices.empty())
        continue;
      watertight(model);
      EXPECT_NEAR(volume(model), trimmed ? 5 * pi / 3 : 32 * pi / 3,
                  trimmed ? .015 : .06);
      expectPolarMeridianJoin(
          model, glm::vec3(sphereMeridianPoint(latitude)),
          [](glm::vec3 p) { return p.x > 0.f && p.y == 0.f; }, 2);
    }
  }
}

TEST(IfcCurvedBrep, SplitSphericalMeridianRejectsAnIncorrectPcurveEndpoint) {
  const auto model =
      LoadFromStep(splitSphericalMeridian(false, 17., false,
                                         SplitMeridianError::WrongPcurve));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(model.elements.empty());
  EXPECT_TRUE(model.vertices.empty());
  EXPECT_TRUE(model.indices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &reason = model.importReport.diagnostics.front().reason;
  EXPECT_NE(reason.find("seam"), std::string::npos) << reason;
  EXPECT_NE(reason.find("pcurve"), std::string::npos) << reason;
}

TEST(IfcCurvedBrep, SplitSphericalMeridianRejectsARimAtAnInteriorJoin) {
  const auto model = LoadFromStep(splitSphericalMeridian(
      true, 57., false, SplitMeridianError::InteriorRim));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(model.vertices.empty());
  EXPECT_TRUE(model.indices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &reason = model.importReport.diagnostics.front().reason;
  EXPECT_NE(reason.find("contiguous closed rim"), std::string::npos) << reason;
}

TEST(IfcCurvedBrep, SphericalEdgeLoopCapThroughPoleRetainsHoleAndDisk) {
  for (bool hole : {false, true}) {
    SCOPED_TRACE(hole);
    CurvedBuilder step;
    constexpr double r = 2., h = .8, holePlane = 1.97;
    const double rho = std::sqrt(r * r - h * h);
    const auto north = step.point({0, 0, r});
    const auto east = step.point({0, rho, h}), west = step.point({0, -rho, h});
    const auto meridian =
        step.add("IFCCIRCLE(" +
                 step.placement({0, 0, 0}, {1, 0, 0}, {0, 1, 0}) + ",2.)");
    const auto toNorth = step.edge(east, north, meridian);
    const auto fromNorth = step.edge(north, west, meridian);
    const auto flatEdge = step.line(east, west);
    const auto latitude = step.add(
        "IFCCIRCLE(" + step.placement({0, 0, h}, {0, 0, 1}, {0, -1, 0}) + "," +
        number(rho) + ")");
    const auto hemisphereRim = step.edge(west, east, latitude);
    const auto surface =
        step.add("IFCSPHERICALSURFACE(" + step.placement({0, 0, 0}) + ",2.)");
    std::vector bounds{step.bound(
        {{toNorth, true}, {fromNorth, true}, {hemisphereRim, true}})};
    if (hole) {
      const double holeRadius = std::sqrt(r * r - holePlane * holePlane);
      // The cutting axis stays 30 degrees from north and the ring's polar
      // opening is less than 10 degrees. The hole touches neither boundary.
      const double angle = pi / 6, s = std::sin(angle), c = std::cos(angle);
      const glm::dvec3 normal(s, 0, c);
      const glm::dvec3 tangent(0, 1, 0);
      const auto cut = step.placement(holePlane * normal, normal, tangent);
      const auto ring =
          step.add("IFCCIRCLE(" + cut + "," + number(holeRadius) + ")");
      const auto ringStart =
          step.point(holePlane * normal + holeRadius * tangent);
      const auto ringEdge = step.edge(ringStart, ringStart, ring);
      bounds.push_back(step.bound({{ringEdge, false}}, false));
      step.plane({{ringEdge, true}}, holePlane * normal, normal);
    }
    step.face(bounds, surface);
    step.plane({{flatEdge, true}, {fromNorth, false}, {toNorth, false}},
               {0, 0, 0}, {-1, 0, 0});
    step.plane({{hemisphereRim, false}, {flatEdge, false}}, {0, 0, h},
               {0, 0, -1});
    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    const auto capVolume = [](double depth) {
      return pi * depth * depth * (r - depth / 3.);
    };
    EXPECT_NEAR(volume(model),
                .5 * capVolume(r - h) - (hole ? capVolume(r - holePlane) : 0.),
                .015);
    std::array<bool, 3> features{};
    const glm::dvec3 cutAxis(.5, 0, std::sqrt(3.) / 2);
    for (const auto &vertex : model.vertices) {
      const glm::dvec3 p(vertex.position), n(vertex.normal);
      if (glm::length(p - glm::dvec3(0, 0, r)) < 1e-6 && n.z > .99999)
        features[0] = true;
      if (glm::dot(n, cutAxis) > .99999 &&
          std::abs(glm::dot(p, cutAxis) - holePlane) < 1e-6)
        features[1] = true;
      if (n.z < -.99999 && std::abs(p.z - h) < 1e-6)
        features[2] = true;
    }
    EXPECT_TRUE(features[0] && features[2]);
    EXPECT_EQ(features[1], hole);
  }
}

TEST(IfcCurvedBrep, SphericalEdgeLoopCavityRetainsInwardWindingAtPoles) {
  CurvedBuilder step;
  const auto outer = explicitSphericalShell(step, 2., true);
  const auto inner = explicitSphericalShell(step, .75, false);
  const auto brep =
      step.add("IFCADVANCEDBREPWITHVOIDS(" + outer + ",(" + inner + "))");
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 4 * pi * (8 - .75 * .75 * .75) / 3, .065);
  std::array<bool, 2> inwardPoles{};
  for (const auto &vertex : model.vertices) {
    const auto p = glm::dvec3(vertex.position);
    const double radius = glm::length(p);
    const bool cavity = radius < 1.;
    EXPECT_NEAR(radius, cavity ? .75 : 2., .0011);
    EXPECT_GT((cavity ? -1 : 1) *
                  glm::dot(p / radius, glm::dvec3(vertex.normal)),
              .99999);
    if (cavity) {
      inwardPoles[0] |= p == glm::dvec3(0, 0, -.75);
      inwardPoles[1] |= p == glm::dvec3(0, 0, .75);
    }
  }
  EXPECT_TRUE(inwardPoles[0] && inwardPoles[1]);
}

struct Paraboloid {
  glm::dvec3 origin, radial, axis, side;
  double scale;

  glm::dvec3 place(glm::dvec3 p) const {
    return origin + scale * (p.x * radial + p.y * axis + p.z * side);
  }

  glm::dvec3 local(glm::dvec3 point) const {
    const auto p = (point - origin) / scale;
    return {glm::dot(p, radial), glm::dot(p, axis), glm::dot(p, side)};
  }

  static std::string profilePoint(CurvedBuilder &step, double x, double y) {
    return step.add("IFCCARTESIANPOINT((" + number(x) + "," + number(y) + "))");
  }

  std::string surfaceFromMeridian(CurvedBuilder &step,
                                   const std::string &curve) const {
    const auto profile =
        step.add("IFCARBITRARYOPENPROFILEDEF(.CURVE.,$," + curve + ")");
    const auto sweepAxis =
        step.add("IFCAXIS1PLACEMENT(" + step.point({0, 0, 0}) + "," +
                 step.direction({0, 1, 0}) + ")");
    return step.add("IFCSURFACEOFREVOLUTION(" + profile + "," +
                    step.placement(origin, side, radial) + "," + sweepAxis +
                    ")");
  }

  std::string surface(CurvedBuilder &step, double middleHeight = 0.,
                      double firstRadius = 0., bool reversed = false) const {
    std::vector controls{profilePoint(step, firstRadius * scale, 0),
                         profilePoint(step, scale, middleHeight * scale),
                         profilePoint(step, 2 * scale, scale)};
    if (reversed)
      std::ranges::reverse(controls);
    const auto curve =
        step.add("IFCBSPLINECURVEWITHKNOTS(2,(" + step.refs(controls) +
                 "),.UNSPECIFIED.,.F.,.F.,(3,3),(2.,5.),.UNSPECIFIED.)");
    return surfaceFromMeridian(step, curve);
  }

  std::string rationalSurface(CurvedBuilder &step, bool reversed) const {
    // The parabola has normalized meridian parameter s = t / (2 - t).
    // One internal knot at t = 1/2 splits its rational quadratic into two
    // spans, while the known surface remains y = r^2 / 4 in local units.
    std::vector controls{profilePoint(step, 0., 0.),
                         profilePoint(step, scale / 3, 0.),
                         profilePoint(step, 4 * scale / 3, scale / 3),
                         profilePoint(step, 2 * scale, scale)};
    if (reversed)
      std::ranges::reverse(controls);
    const std::string weights = reversed ? "(0.25,0.375,0.75,1.)"
                                         : "(1.,0.75,0.375,0.25)";
    const auto curve = step.add(
        "IFCRATIONALBSPLINECURVEWITHKNOTS(2,(" + step.refs(controls) +
        "),.UNSPECIFIED.,.F.,.F.,(3,1,3),(2.,3.5,5.),.UNSPECIFIED.," +
        weights + ")");
    return surfaceFromMeridian(step, curve);
  }

  std::string meridian(CurvedBuilder &step, const std::string &apex,
                       const std::string &rim, glm::dvec3 direction) const {
    const auto middle = step.point(origin + scale * direction);
    const auto curve = step.add(
        "IFCBSPLINECURVEWITHKNOTS(2,(" + step.refs({apex, middle, rim}) +
        "),.UNSPECIFIED.,.F.,.F.,(3,3),(2.,5.),.UNSPECIFIED.)");
    return step.edge(apex, rim, curve);
  }
};

const Paraboloid standardParaboloid{
    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1.};

void expectParaboloidNormals(const Model &model, const Paraboloid &shape,
                             bool outward) {
  bool pole = false, disk = false;
  for (const auto &vertex : model.vertices) {
    const auto p = shape.local(vertex.position);
    const glm::dvec3 normal(vertex.normal);
    const double sign = outward ? 1. : -1.;
    if (std::abs(p.y - 1.) < 1e-6 &&
        sign * glm::dot(normal, shape.axis) > .99999) {
      disk = true;
      continue;
    }
    EXPECT_NEAR(p.y, (p.x * p.x + p.z * p.z) / 4, 2e-6);
    const auto expected = glm::normalize(p.x / 2 * shape.radial - shape.axis +
                                         p.z / 2 * shape.side);
    EXPECT_GT(sign * glm::dot(normal, expected), .9999);
    pole |= glm::length(p) < 1e-7;
  }
  EXPECT_TRUE(pole && disk);
}

std::string addParaboloidShell(CurvedBuilder &step, const Paraboloid &shape,
                               bool vertexLoop, bool outward,
                               bool rationalMeridian = false) {
  const auto surface = rationalMeridian ? shape.rationalSurface(step, vertexLoop)
                                       : shape.surface(step, 0., 0., vertexLoop);
  const auto apex = step.point(shape.origin);
  const auto ring = step.add(
      "IFCCIRCLE(" +
      step.placement(shape.place({0, 1, 0}), shape.axis, shape.radial) + "," +
      number(2 * shape.scale) + ")");
  const auto rimA = step.point(shape.place({2, 1, 0}));
  const auto firstFace = step.faces.size();
  const bool reversed = !outward;
  if (vertexLoop) {
    const auto rim = step.edge(rimA, rimA, ring);
    step.face({step.vertexBound(shape.origin, false),
               step.bound({{rim, reversed}}, true, false)},
              surface, !outward);
    step.plane({{rim, outward}}, shape.place({0, 1, 0}),
               (outward ? 1. : -1.) * shape.axis);
  } else {
    const auto rimB = step.point(shape.place({-2, 1, 0}));
    const auto a = shape.meridian(step, apex, rimA, shape.radial);
    const auto b = shape.meridian(step, apex, rimB, -shape.radial);
    const auto rAB = step.edge(rimA, rimB, ring),
               rBA = step.edge(rimB, rimA, ring);
    step.face(
        {step.bound({{b, true}, {rAB, false}, {a, false}}, true, outward)},
        surface, outward);
    step.face(
        {step.bound({{a, true}, {rBA, false}, {b, false}}, true, outward)},
        surface, outward);
    step.plane({{rAB, outward}, {rBA, outward}}, shape.place({0, 1, 0}),
               (outward ? 1. : -1.) * shape.axis);
  }
  const std::vector<std::string> faces(step.faces.begin() + firstFace,
                                       step.faces.end());
  return step.add("IFCCLOSEDSHELL((" + step.refs(faces) + "))");
}

struct RevolvedEllipsoid {
  glm::dvec3 center;
  double equatorialRadius, polarRadius;
  static constexpr glm::dvec3 radial{1, 0, 0}, axis{0, .6, .8};

  std::string shell(CurvedBuilder &step, bool outward, bool reversed = false,
                    bool offsetAxis = false) const {
    const bool prolate = polarRadius > equatorialRadius;
    const double a = std::max(equatorialRadius, polarRadius);
    const double b = std::min(equatorialRadius, polarRadius);
    const auto origin2d = step.add("IFCCARTESIANPOINT((0.,0.))");
    const auto direction2d = step.add(prolate ? "IFCDIRECTION((0.,1.))"
                                             : "IFCDIRECTION((1.,0.))");
    const auto plane =
        step.add("IFCAXIS2PLACEMENT2D(" + origin2d + "," + direction2d + ")");
    const auto ellipse = step.add("IFCELLIPSE(" + plane + "," + number(a) +
                                  "," + number(b) + ")");
    const double lower = prolate ? 180. : -90., upper = lower + 180.;
    const auto trim = step.add(
        "IFCTRIMMEDCURVE(" + ellipse + ",(IFCPARAMETERVALUE(" +
        number(reversed ? upper : lower) + ")),(IFCPARAMETERVALUE(" +
        number(reversed ? lower : upper) + "))," +
        (reversed ? ".F." : ".T.") + ",.PARAMETER.)");
    const auto profile = step.add("IFCARBITRARYOPENPROFILEDEF(.CURVE.,$," + trim + ")");
    const auto axisOrigin = step.point({offsetAxis ? .05 : 0., 0, 0});
    const auto axisPlacement = step.add("IFCAXIS1PLACEMENT(" + axisOrigin +
                                        "," + step.direction({0, 1, 0}) + ")");
    const auto normal = glm::cross(radial, axis);
    const auto sweptFrame = step.placement(center, normal, radial);
    const auto surface = step.add("IFCSURFACEOFREVOLUTION(" + profile + "," +
                                  sweptFrame + "," + axisPlacement + ")");

    // Both poles lie inside the same native conic trim, away from its
    // adaptive dyadic samples. The two faces use its one edge in reverse.
    const auto edgeFrame = step.placement(center, normal, prolate ? axis : radial);
    const auto meridian = step.add("IFCELLIPSE(" + edgeFrame + "," + number(a) +
                                    "," + number(b) + ")");
    constexpr double startAngle = .137;
    const auto start = step.point(center + equatorialRadius * std::cos(startAngle) * radial +
                                  polarRadius * std::sin(startAngle) * axis);
    const auto edge = step.edge(start, start, meridian);
    for (const bool forward : {true, false})
      step.face({step.bound({{edge, forward}})}, surface, outward != reversed);
    return step.add("IFCCLOSEDSHELL((" + step.faces[step.faces.size() - 2] + "," +
                    step.faces.back() + "))");
  }

  double volume() const {
    return (4 * pi / 3) * equatorialRadius * equatorialRadius * polarRadius;
  }

  size_t checkNormalsAndPoleEdges(const Model &model, bool outward) const {
    const auto meridianNormal = glm::cross(radial, axis);
    for (const double sign : {-1., 1.}) {
      const auto pole = glm::vec3(center + sign * polarRadius * axis);
      expectPolarMeridianJoin(model, pole, [&](glm::vec3 p) {
        return std::abs(glm::dot(glm::dvec3(p) - center, meridianNormal)) < 1e-6;
      }, 0);
    }
    size_t count = 0;
    for (const auto &vertex : model.vertices) {
      const auto p = glm::dvec3(vertex.position) - center;
      const auto height = glm::dot(p, axis) * axis;
      const auto equatorial = p - height;
      const double equatorialSquared = equatorialRadius * equatorialRadius;
      const double axialSquared = polarRadius * polarRadius;
      const double level = glm::dot(equatorial, equatorial) / equatorialSquared +
                           glm::dot(height, height) / axialSquared;
      if (std::abs(level - 1.) > 1e-5)
        continue;
      ++count;
      const auto normal = glm::normalize(equatorial / equatorialSquared +
                                         height / axialSquared);
      EXPECT_GT((outward ? 1. : -1.) * glm::dot(normal, glm::dvec3(vertex.normal)),
                .99999);
    }
    EXPECT_GT(count, 100u);
    return count;
  }
};

TEST(IfcCurvedBrep, RevolvedHalfEllipseSharesAnExactMeridianAtBothPoles) {
  const RevolvedEllipsoid outer{{.25, -.5, .75}, 1.6, 2.4};
  const RevolvedEllipsoid cavity{outer.center + .2 * RevolvedEllipsoid::axis,
                                  .9, .55};
  for (const bool withCavity : {false, true}) {
    SCOPED_TRACE(withCavity);
    CurvedBuilder step;
    const auto exterior = outer.shell(step, true);
    std::string brep;
    if (withCavity) {
      const auto interior = cavity.shell(step, false, true);
      brep = step.add("IFCADVANCEDBREPWITHVOIDS(" + exterior + ",(" + interior + "))");
    } else {
      brep = step.add("IFCADVANCEDBREP(" + exterior + ")");
    }
    const auto source = fixture(step.definitions + degrees(), brep);
    if (withCavity)
      exportCurvedFixture("revolved-ellipsoid-poles-cavity", source);
    const auto model = LoadFromStep(source);
    ASSERT_NO_FATAL_FAILURE(complete(model));
    watertight(model);
    EXPECT_NEAR(volume(model), outer.volume() - (withCavity ? cavity.volume() : 0.), .055);
    auto vertices = outer.checkNormalsAndPoleEdges(model, true);
    if (withCavity)
      vertices += cavity.checkNormalsAndPoleEdges(model, false);
    EXPECT_EQ(vertices, model.vertices.size());
  }
}

TEST(IfcCurvedBrep, RevolvedHalfEllipseRejectsAnOffsetAxisAtomically) {
  const RevolvedEllipsoid shape{{0, 0, 0}, 1.6, 2.4};
  CurvedBuilder step;
  const auto shell = shape.shell(step, true, false, true);
  const auto brep = step.add("IFCADVANCEDBREP(" + shell + ")");
  const auto model = LoadFromStep(fixture(step.definitions + degrees(), brep));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(model.elements.empty());
  EXPECT_TRUE(model.vertices.empty());
  EXPECT_TRUE(model.indices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  EXPECT_NE(model.importReport.diagnostics.front().reason.find("Singular revolution"),
            std::string::npos);
}

TEST(IfcCurvedBrep, SmoothRevolutionApexClosesParaboloidSectorsAndVertexLoop) {
  for (bool vertexLoop : {false, true}) {
    SCOPED_TRACE(vertexLoop);
    CurvedBuilder step;
    const auto shape = vertexLoop ? Paraboloid{{3000, 5000, -2000},
                                               {0, 1, 0},
                                               {0, 0, 1},
                                               {1, 0, 0},
                                               1000.}
                                  : standardParaboloid;
    const auto shell = addParaboloidShell(step, shape, vertexLoop, true);
    const auto brep = step.add("IFCADVANCEDBREP(" + shell + ")");
    auto source = fixture(step.definitions + degrees(), brep);
    if (vertexLoop) {
      constexpr std::string_view metres = ".LENGTHUNIT.,$,.METRE.";
      source.replace(source.find(metres), metres.size(),
                     ".LENGTHUNIT.,.MILLI.,.METRE.");
    }
    const auto model = LoadFromStep(source);
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model) / std::pow(shape.scale, 3.), 2 * pi, .025);
    expectParaboloidNormals(model, shape, true);
    exportCurvedFixture(vertexLoop ? "revolution-apex-millimetres"
                                   : "revolution-apex-sectors",
                        source);
  }
}

TEST(IfcCurvedBrep, SmoothRationalMultiSpanRevolutionPreservesParaboloidShell) {
  for (bool reversed : {false, true}) {
    SCOPED_TRACE(reversed);
    CurvedBuilder step;
    const Paraboloid shape = reversed ? Paraboloid{{3000, 5000, -2000},
                                                   {0, 1, 0},
                                                   {0, 0, 1},
                                                   {1, 0, 0}, 1000.}
                                      : standardParaboloid;
    const auto shell = addParaboloidShell(step, shape, reversed, true, true);
    const auto brep = step.add("IFCADVANCEDBREP(" + shell + ")");
    auto source = fixture(step.definitions + degrees(), brep);
    if (reversed) {
      constexpr std::string_view metres = ".LENGTHUNIT.,$,.METRE.";
      source.replace(source.find(metres), metres.size(),
                     ".LENGTHUNIT.,.MILLI.,.METRE.");
    }
    const auto model = LoadFromStep(source);
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model) / std::pow(shape.scale, 3.), 2 * pi, .025);
    expectParaboloidNormals(model, shape, true);
    exportCurvedFixture(reversed ? "rational-revolution-apex-millimetres"
                                  : "rational-revolution-apex-sectors",
                        source);
  }
}

TEST(IfcCurvedBrep, SmoothRevolutionApexIsInsertedInsideASharedSplineEdge) {
  CurvedBuilder step;
  const auto surface = standardParaboloid.surface(step);
  const auto a = step.point({-2, 1, 0}), b = step.point({2, 1, 0});
  // The spline crosses the axis at a fraction 2/3.3 of its first native
  // knot span, away from dyadic seeds, and continues along the same parabola.
  const auto c = step.point({1.3, 1.3 * 1.3 / 4, 0});
  const auto firstControls = step.refs({a, step.point({-.35, -.65, 0}), c});
  const auto secondControls = step.refs({c, step.point({1.65, .65, 0}), b});
  const auto spline = [&](const std::string &controls, double from, double to) {
    return step.add("IFCBSPLINECURVEWITHKNOTS(2,(" + controls +
                    "),.UNSPECIFIED.,.F.,.F.,(3,3),(" + number(from) + "," +
                    number(to) + "),.UNSPECIFIED.)");
  };
  const auto left = spline(firstControls, 2., 3.1);
  const auto right = spline(secondControls, 3.1, 4.);
  const auto first =
      step.add("IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.," + left + ")");
  const auto last =
      step.add("IFCCOMPOSITECURVESEGMENT(.DISCONTINUOUS.,.T.," + right + ")");
  const auto meridian =
      step.add("IFCCOMPOSITECURVE((" + first + "," + last + "),.F.)");
  const auto across = step.edge(a, b, meridian);
  const auto circle =
      step.add("IFCCIRCLE(" + step.placement({0, 1, 0}, {0, 1, 0}) + ",2.)");
  const auto rimAB = step.edge(a, b, circle), rimBA = step.edge(b, a, circle);
  step.face({step.bound({{across, true}, {rimAB, false}})}, surface);
  step.face({step.bound({{across, false}, {rimBA, false}})}, surface);
  step.plane({{rimAB, true}, {rimBA, true}}, {0, 1, 0}, {0, 1, 0});
  const auto brep = step.finish();
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 2 * pi, .025);
  expectParaboloidNormals(model, standardParaboloid, true);
  EXPECT_TRUE(std::ranges::any_of(model.vertices, [](const auto &vertex) {
    return vertex.position == glm::vec3(0) &&
           vertex.normal == glm::vec3(0, -1, 0);
  }));
}

std::string paraboloidMeridianSeam(CurvedBuilder &step,
                                    const std::string &surface,
                                    const std::string &apex,
                                    const std::string &rim,
                                    double endLongitude = 360.) {
  const auto meridian =
      step.add("IFCBSPLINECURVEWITHKNOTS(2,(" +
               step.refs({apex, step.point({1, 0, 0}), rim}) +
               "),.UNSPECIFIED.,.F.,.F.,(3,3),(2.,5.),.UNSPECIFIED.)");
  const auto pcurve = [&](double u) {
    const auto a = Paraboloid::profilePoint(step, u, 2.);
    const auto b = Paraboloid::profilePoint(step, u, 5.);
    const auto uv = step.add("IFCPOLYLINE((" + a + "," + b + "))");
    return step.add("IFCPCURVE(" + surface + "," + uv + ")");
  };
  const auto seam =
      step.add("IFCSEAMCURVE(" + meridian + ",(" + pcurve(0) + "," +
               pcurve(endLongitude) + "),.CURVE3D.)");
  return step.edge(apex, rim, seam);
}

TEST(IfcCurvedBrep, SmoothRevolutionApexPreservesSharedMeridianSeam) {
  for (bool invalid : {false, true}) {
    SCOPED_TRACE(invalid);
    CurvedBuilder step;
    const auto surface = standardParaboloid.surface(step);
    const auto apex = step.point({0, 0, 0}), rim = step.point({2, 1, 0});
    const auto seamEdge = paraboloidMeridianSeam(step, surface, apex, rim,
                                                 invalid ? 180. : 360.);
    const auto rimCircle =
        step.add("IFCCIRCLE(" + step.placement({0, 1, 0}, {0, 1, 0}) + ",2.)");
    const auto rimEdge = step.edge(rim, rim, rimCircle);
    step.face(
        {step.bound({{seamEdge, true}, {rimEdge, false}, {seamEdge, false}})},
        surface);
    step.plane({{rimEdge, true}}, {0, 1, 0}, {0, 1, 0});
    const auto brep = step.finish();
    const auto source = fixture(step.definitions + degrees(), brep);
    const auto model = LoadFromStep(source);
    if (invalid) {
      EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
      EXPECT_TRUE(model.indices.empty());
      EXPECT_TRUE(model.vertices.empty());
      ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
      EXPECT_NE(model.importReport.diagnostics.front().reason.find("seam"),
                std::string::npos)
          << model.importReport.diagnostics.front().reason;
      continue;
    }
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model), 2 * pi, .025);
    expectParaboloidNormals(model, standardParaboloid, true);
    std::set<float> seamHeights;
    for (const auto &vertex : model.vertices) {
      const auto p = vertex.position;
      if (p.x > 0 && p.z == 0 && p.y > 0 && p.y < 1)
        seamHeights.insert(p.y);
    }
    EXPECT_GE(seamHeights.size(), 16u);
    exportCurvedFixture("revolution-apex-seam", source);
  }
}

TEST(IfcCurvedBrep, SmoothRevolutionApexPreservesSplitMeridianJoin) {
  CurvedBuilder step;
  constexpr double t = .39;
  const glm::dvec3 joinPosition(2 * t, t * t, 0);
  const auto surface = standardParaboloid.surface(step);
  const auto apex = step.point({0, 0, 0}), join = step.point(joinPosition),
             rim = step.point({2, 1, 0});
  const auto segment = [&](const std::string &start, const std::string &end,
                           double a, double b) {
    // Restrict the authored meridian P(t)=(2t,t^2,0) to the span [a,b].
    const auto middle = step.point({a + b, a * b, 0});
    const double firstV = 2 + 3 * a, lastV = 2 + 3 * b;
    const auto curve = step.add(
        "IFCBSPLINECURVEWITHKNOTS(2,(" + step.refs({start, middle, end}) +
        "),.UNSPECIFIED.,.F.,.F.,(3,3),(" + number(firstV) + "," +
        number(lastV) + "),.UNSPECIFIED.)");
    return polarMeridianEdge(step, surface, curve, start, end, firstV, lastV,
                             {});
  };
  const auto inner = segment(apex, join, 0., t),
             outer = segment(join, rim, t, 1.);
  const auto rimCircle =
      step.add("IFCCIRCLE(" + step.placement({0, 1, 0}, {0, 1, 0}) + ",2.)");
  const auto rimEdge = step.edge(rim, rim, rimCircle);
  step.face({step.bound({{inner, true},
                         {outer, true},
                         {rimEdge, false},
                         {outer, false},
                         {inner, false}})},
            surface);
  step.plane({{rimEdge, true}}, {0, 1, 0}, {0, 1, 0});
  const auto brep = step.finish();
  const auto source = fixture(step.definitions + degrees(), brep);
  exportCurvedFixture("revolution-apex-split-seam", source);
  const auto model = LoadFromStep(source);
  ASSERT_NO_FATAL_FAILURE(complete(model));
  watertight(model);
  EXPECT_NEAR(volume(model), 2 * pi, .025);
  expectParaboloidNormals(model, standardParaboloid, true);
  expectPolarMeridianJoin(
      model, glm::vec3(joinPosition),
      [](glm::vec3 p) { return p.x > 0.f && p.z == 0.f; }, 0);
}

TEST(IfcCurvedBrep, SmoothRevolutionKeepsNativeAngleAndMeridianAtApex) {
  const std::array cases{std::pair(0., false), std::pair(90., false),
                         std::pair(0., true), std::pair(90., true)};
  for (const auto &[angle, rational] : cases) {
    SCOPED_TRACE(::testing::Message() << angle << ", rational=" << rational);
    CurvedBuilder step;
    const auto surface = rational ? standardParaboloid.rationalSurface(step, false)
                                  : standardParaboloid.surface(step);
    const auto uvStart = Paraboloid::profilePoint(step, angle, 2.);
    const auto uvMiddle = Paraboloid::profilePoint(step, angle, 3.5);
    const auto uvEnd = Paraboloid::profilePoint(step, angle, 5.);
    const auto curve = step.add("IFCPOLYLINE((" + uvStart + "," + uvMiddle +
                                "," + uvEnd + "))");
    const auto pcurve = step.add("IFCPCURVE(" + surface + "," + curve + ")");
    const auto model = LoadFromStep(
        fixture(step.definitions + degrees(), pcurve, "'Axis','Curve3D'"));
    complete(model);
    ASSERT_FALSE(model.nativeCurveRanges.empty());
    const auto &line = model.nativeCurveRanges.front();
    const auto point = [&](size_t i) {
      return glm::dvec3(model.vertices.at(model.indices.at(i)).position);
    };
    EXPECT_EQ(point(line.firstIndex), glm::dvec3(0));
    const glm::dvec3 radial =
        angle == 90. ? glm::dvec3(0, 0, -1) : glm::dvec3(1, 0, 0);
    EXPECT_LT(glm::length(point(line.firstIndex + line.indexCount - 1) -
                          (2. * radial + glm::dvec3(0, 1, 0))),
              1e-6);
    const double middleRadius = rational ? 2. / 3. : 1.;
    const auto expectedMiddle = middleRadius * radial +
                                glm::dvec3(0, middleRadius * middleRadius / 4., 0);
    bool foundMiddle = false;
    for (size_t i = line.firstIndex;
         i < size_t(line.firstIndex) + line.indexCount; ++i) {
      const auto p = point(i);
      const double r = glm::dot(p, radial);
      EXPECT_NEAR(p.y, r * r / 4, 1e-6);
      EXPECT_LE(glm::length(p - r * radial - glm::dvec3(0, p.y, 0)), 1e-6);
      foundMiddle |= glm::length(p - expectedMiddle) < 1e-6;
    }
    EXPECT_TRUE(foundMiddle);
  }
}

TEST(IfcCurvedBrep, SmoothRevolutionApexSupportsAnInwardParaboloidCavity) {
  CurvedBuilder step;
  const Paraboloid interior{{0, .25, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, .5};
  const auto outer = addParaboloidShell(step, standardParaboloid, true, true);
  const auto inner = addParaboloidShell(step, interior, true, false);
  const auto brep =
      step.add("IFCADVANCEDBREPWITHVOIDS(" + outer + ",(" + inner + "))");
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 2 * pi * (1. - .125), .03);
  bool outwardApex = false, inwardApex = false, inwardDisk = false;
  for (const auto &vertex : model.vertices) {
    const auto p = vertex.position;
    outwardApex |= p == glm::vec3(0) && vertex.normal.y < -.99999;
    inwardApex |= p == glm::vec3(0, .25, 0) && vertex.normal.y > .99999;
    inwardDisk |= p.y == .75f && vertex.normal.y < -.99999;
  }
  EXPECT_TRUE(outwardApex && inwardApex && inwardDisk);
}

TEST(IfcCurvedBrep, SmoothRevolutionApexRetainsOffAxisHoleAndSharedBoundary) {
  for (bool authoredSeam : {false, true}) {
    SCOPED_TRACE(authoredSeam);
    CurvedBuilder step;
    const auto surface = standardParaboloid.surface(step);
    const auto apex = step.point({0, 0, 0});
    const auto outerA = step.point({2, 1, 0});
    const auto outerRing =
        step.add("IFCCIRCLE(" + step.placement({0, 1, 0}, {0, 1, 0}) + ",2.)");
    // Contrasting faces share an off-axis patch boundary. One face trims this
    // exact parametric box away; a second B-rep face supplies the same patch.
    // The four authored edges are exact circles and quadratic meridians.
    constexpr double r0 = .6, r1 = 1.3;
    const double t0 = authoredSeam ? 4 * pi / 5 : pi / 5;
    const double t1 = authoredSeam ? 6 * pi / 5 : 2 * pi / 5;
    const auto radial = [](double t) {
      return glm::dvec3(std::cos(t), 0, std::sin(t));
    };
    const auto radialA = radial(t0), radialB = radial(t1);
    const auto at = [](double radius, glm::dvec3 radial) {
      return radius * radial + glm::dvec3(0, radius * radius / 4, 0);
    };
    const auto p0 = step.point(at(r0, radialA)), p1 = step.point(at(r1, radialA)),
               p2 = step.point(at(r1, radialB)), p3 = step.point(at(r0, radialB));
    const auto meridian = [&](const std::string &first, const std::string &last,
                              glm::dvec3 direction) {
      const double inner = r0 * r0 / 4 + r0 * (r1 - r0) / 4;
      const auto middle =
          step.point((r0 + r1) / 2 * direction + glm::dvec3(0, inner, 0));
      const auto curve = step.add(
          "IFCBSPLINECURVEWITHKNOTS(2,(" + step.refs({first, middle, last}) +
          "),.UNSPECIFIED.,.F.,.F.,(3,3),(2.,5.),.UNSPECIFIED.)");
      return step.edge(first, last, curve);
    };
    const auto latitude = [&](double radius, const std::string &first,
                              const std::string &last) {
      const auto circle = step.add(
          "IFCCIRCLE(" + step.placement({0, radius * radius / 4, 0}, {0, 1, 0}) +
          "," + number(radius) + ")");
      return step.edge(first, last, circle);
    };
    const auto pa = meridian(p0, p1, radialA), pb = meridian(p3, p2, radialB),
               ra = latitude(r0, p3, p0), rb = latitude(r1, p2, p1);
    const auto hole = step.bound({{pa, false}, {ra, false}, {pb, true}, {rb, true}},
                                 false);
    if (authoredSeam) {
      // The off-axis patch straddles the old generated -X meridian.
      const auto seam = paraboloidMeridianSeam(step, surface, apex, outerA);
      const auto rim = step.edge(outerA, outerA, outerRing);
      step.face({step.bound({{seam, true}, {rim, false}, {seam, false}}), hole},
                surface);
      step.plane({{rim, true}}, {0, 1, 0}, {0, 1, 0});
    } else {
      const auto outerB = step.point({-2, 1, 0});
      const auto a = standardParaboloid.meridian(step, apex, outerA, {1, 0, 0});
      const auto b = standardParaboloid.meridian(step, apex, outerB, {-1, 0, 0});
      const auto upper = step.edge(outerA, outerB, outerRing),
                 lower = step.edge(outerB, outerA, outerRing);
      step.face({step.bound({{b, true}, {upper, false}, {a, false}})}, surface);
      step.face({step.bound({{a, true}, {lower, false}, {b, false}}), hole},
                surface);
      step.plane({{upper, true}, {lower, true}}, {0, 1, 0}, {0, 1, 0});
    }
    step.face({step.bound({{pa, true}, {rb, false}, {pb, false}, {ra, true}})},
              surface);
    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions + degrees(), brep));
    complete(model);
    ASSERT_FALSE(model.indices.empty());
    watertight(model);
    EXPECT_NEAR(volume(model), 2 * pi, .025);
    expectParaboloidNormals(model, standardParaboloid, true);
  }
}

TEST(IfcCurvedBrep, MalformedRevolutionApexAndAxisCrossingRejectAtomically) {
  for (unsigned variant : {0u, 1u, 2u}) {
    SCOPED_TRACE(variant);
    CurvedBuilder step;
    const auto surface = standardParaboloid.surface(
        step, variant == 0 ? .4 : 0., variant == 1 ? -.25 : 0.);
    const auto circle =
        step.add("IFCCIRCLE(" + step.placement({0, 1, 0}, {0, 1, 0}) + ",2.)");
    const auto onRim = step.point({2, 1, 0});
    const auto rim = step.edge(onRim, onRim, circle);
    const auto vertex = variant == 2 ? glm::dvec3(0, 0.1, 0) : glm::dvec3(0);
    step.face({step.vertexBound(vertex, false), step.bound({{rim, false}})},
              surface);
    step.plane({{rim, true}}, {0, 1, 0}, {0, 1, 0});
    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.indices.empty());
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.meshRanges.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_FALSE(model.importReport.diagnostics.front().reason.empty());
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

TEST(IfcCurvedBrep, HollowSweepBoundsSelectOppositeWallChartsOfOneSurface) {
  CurvedBuilder step;
  const auto outerBottom = step.circle(3, 0), outerTop = step.circle(3, 4);
  const auto innerBottom = step.circle(2, 0), innerTop = step.circle(2, 4);
  const auto profile = step.add("IFCCIRCLEHOLLOWPROFILEDEF(.CURVE.,$,$,3.,1.)");
  const auto surface = step.add("IFCSURFACEOFLINEAREXTRUSION(" + profile +
                                ",$," + step.direction({0, 0, 1}) + ",4.)");
  const auto addWall = [&](const CurvedBuilder::Circle &bottom,
                           const CurvedBuilder::Circle &top, bool outer) {
    const auto pcurve = [&](double angle) {
      const auto u = number(angle);
      const auto a = step.add("IFCCARTESIANPOINT((" + u + ",0.))"),
                 b = step.add("IFCCARTESIANPOINT((" + u + ",1.))");
      return step.add("IFCPCURVE(" + surface + "," +
                      step.add("IFCPOLYLINE((" + a + "," + b + "))") + ")");
    };
    const auto curve = step.add(
        "IFCSEAMCURVE(" +
        step.add("IFCPOLYLINE((" + bottom.point + "," + top.point + "))") +
        ",(" + pcurve(0) + "," + pcurve(2 * pi) + "),.CURVE3D.)");
    const auto seam = step.edge(bottom.point, top.point, curve);
    const std::vector<CurvedBuilder::Use> uses =
        outer ? std::vector<CurvedBuilder::Use>{{bottom.edge, true},
                                                {seam, true},
                                                {top.edge, false},
                                                {seam, false}}
              : std::vector<CurvedBuilder::Use>{{seam, true},
                                                {top.edge, true},
                                                {seam, false},
                                                {bottom.edge, false}};
    step.face({step.bound(uses)}, surface, outer);
  };
  addWall(outerBottom, outerTop, true);
  addWall(innerBottom, innerTop, false);
  step.face({step.bound({{outerTop.edge, true}}),
             step.bound({{innerTop.edge, false}}, false)},
            step.add("IFCPLANE(" + step.placement({0, 0, 4}) + ")"));
  step.face(
      {step.bound({{outerBottom.edge, false}}),
       step.bound({{innerBottom.edge, true}}, false)},
      step.add("IFCPLANE(" + step.placement({0, 0, 0}, {0, 0, -1}) + ")"));
  const auto brep = step.finish();
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  complete(model);
  ASSERT_FALSE(model.indices.empty());
  watertight(model);
  EXPECT_NEAR(volume(model), 20 * pi, .05);
  std::array<size_t, 2> sideVertices{};
  for (const auto &vertex : model.vertices) {
    if (std::abs(vertex.normal.z) > .5)
      continue;
    const double radius = std::hypot(vertex.position.x, vertex.position.y);
    const size_t wall = radius < 2.5 ? 0 : 1;
    EXPECT_NEAR(radius, wall == 0 ? 2. : 3., 1e-5);
    const auto radial = glm::normalize(glm::dvec2(vertex.position));
    EXPECT_GT((wall == 0 ? -1. : 1.) *
                  glm::dot(glm::dvec2(vertex.normal), radial),
              .999);
    ++sideVertices[wall];
  }
  EXPECT_GT(sideVertices[0], 0u);
  EXPECT_GT(sideVertices[1], 0u);
}

TEST(IfcCurvedBrep, CenterlineSweepRetainsSharpCreaseAcrossTwoWalls) {
  for (const bool omitCorner : {false, true}) {
    SCOPED_TRACE(omitCorner);
    CurvedBuilder step;
    const auto a = step.point({0, 0, 0}), b = step.point({3, 0, 0}),
               c = step.point({3, 2, 0}), d = step.point({0, 2, 0});
    const auto A = step.point({0, 0, 4}), B = step.point({3, 0, 4}),
               C = step.point({3, 2, 4}), D = step.point({0, 2, 4});
    const auto parameterPoint = [&](int x, int y) {
      return step.add("IFCCARTESIANPOINT((" + std::to_string(x) + "," +
                      std::to_string(y) + "))");
    };
    const auto profileCurve =
        step.add("IFCPOLYLINE((" +
                 step.refs({parameterPoint(0, 0), parameterPoint(3, 0),
                            parameterPoint(3, 2)}) +
                 "))");
    const auto profile =
        step.add("IFCCENTERLINEPROFILEDEF(.CURVE.,$," + profileCurve + ",1.)");
    const auto surface = step.add("IFCSURFACEOFLINEAREXTRUSION(" + profile +
                                  ",$," + step.direction({0, 0, 1}) + ",4.)");
    const auto bottom = omitCorner
                            ? step.line(a, c)
                            : step.edge(a, c,
                                        step.add("IFCPOLYLINE((" +
                                                 step.refs({a, b, c}) + "))"));
    const auto top = step.edge(
        A, C, step.add("IFCPOLYLINE((" + step.refs({A, B, C}) + "))"));
    const auto cd = step.line(c, d), da = step.line(d, a), CD = step.line(C, D),
               DA = step.line(D, A), aA = step.line(a, A), cC = step.line(c, C),
               dD = step.line(d, D);
    step.face(
        {step.bound({{bottom, true}, {cC, true}, {top, false}, {aA, false}})},
        surface);
    step.plane({{bottom, false}, {da, false}, {cd, false}}, {0, 0, 0},
               {0, 0, -1});
    step.plane({{top, true}, {CD, true}, {DA, true}}, {0, 0, 4}, {0, 0, 1});
    step.plane({{cd, true}, {dD, true}, {CD, false}, {cC, false}}, {0, 2, 0},
               {0, 1, 0});
    step.plane({{da, true}, {aA, true}, {DA, false}, {dD, false}}, {0, 0, 0},
               {-1, 0, 0});

    const auto brep = step.finish();
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    if (omitCorner) {
      EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
      EXPECT_TRUE(model.vertices.empty());
      EXPECT_TRUE(model.indices.empty());
      ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
      EXPECT_NE(model.importReport.diagnostics.front().reason.find("Face #"),
                std::string::npos);
      continue;
    }
    complete(model);
    watertight(model);
    EXPECT_NEAR(volume(model), 24., 1e-5);

    // A triangle across the crease would have vertices in both planes, but
    // all three must belong to one face of the rectangular prism.
    const std::array<glm::dvec3, 6> normals{
        {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
    const std::array<double, 6> offsets{0., 3., 0., 2., 0., 4.};
    std::array<double, 6> areas{};
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      std::array<glm::dvec3, 3> p;
      for (size_t j = 0; j < p.size(); ++j)
        p[j] = glm::dvec3(model.vertices.at(model.indices.at(i + j)).position);
      const auto n = glm::cross(p[1] - p[0], p[2] - p[0]);
      bool inPlane = false;
      for (size_t plane = 0; plane < normals.size(); ++plane) {
        if (std::ranges::all_of(p, [&](glm::dvec3 q) {
              return std::abs(glm::dot(q, normals[plane]) - offsets[plane]) <
                     1e-6;
            })) {
          EXPECT_GT(glm::dot(n, normals[plane]), 0.);
          areas[plane] += .5 * glm::length(n);
          inPlane = true;
          break;
        }
      }
      EXPECT_TRUE(inPlane) << "Triangle " << i / 3 << " crosses a prism corner";
    }
    const std::array<double, 6> expected{8., 8., 12., 12., 6., 6.};
    for (size_t i = 0; i < areas.size(); ++i)
      EXPECT_NEAR(areas[i], expected[i], 1e-5);
    std::map<float, std::pair<bool, bool>> crease;
    for (const auto &vertex : model.vertices) {
      const auto p = vertex.position;
      if (p.x == 3. && p.y == 0.) {
        auto &normalsAtCrease = crease[p.z];
        normalsAtCrease.first |= vertex.normal.x > .9999;
        normalsAtCrease.second |= vertex.normal.y < -.9999;
      }
    }
    ASSERT_TRUE(crease.contains(0.f));
    ASSERT_TRUE(crease.contains(4.f));
    for (const auto &[z, normalsAtCrease] : crease) {
      EXPECT_TRUE(normalsAtCrease.first) << z;
      EXPECT_TRUE(normalsAtCrease.second) << z;
    }
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

struct SplineAxis {
  double low, high;
  bool periodic;

  std::string knots() const {
    std::string result = "(" + number(low);
    if (periodic)
      for (int i = 1; i < 4; ++i)
        result += "," + number(low + (high - low) * i / 4);
    return result + "," + number(high) + ")";
  }

  const char *multiplicities() const {
    return periodic ? "(3,2,2,2,3)" : "(2,2)";
  }

  double weight(size_t control) const {
    return periodic && control % 2 != 0 ? std::sqrt(.5) : 1.;
  }

  size_t controls() const { return periodic ? 9u : 2u; }
  const char *degree() const { return periodic ? "2" : "1"; }
  const char *closed() const { return periodic ? ".T." : ".F."; }
};

// Four exact quadratic rational quarter circles. Odd indices are tangent
// intersections, rather than points lying on the circle.
const std::array<glm::dvec2, 9> circleControls{{{1, 0},
                                                {1, 1},
                                                {0, 1},
                                                {-1, 1},
                                                {-1, 0},
                                                {-1, -1},
                                                {0, -1},
                                                {1, -1},
                                                {1, 0}}};

enum class SplineLift {
  Valid,
  WrongRing,
  AmbiguousRing,
  DuplicateSeam,
  TwiceSeam,
  MixedRings,
  MixedSeam,
  MixedConflict
};
enum class DetachedSplineTrim { None, BareHole, BareOuter, HoleAtSeam };

struct PeriodicSplineBuilder : CurvedBuilder {
  static constexpr SplineAxis torusU{2.75, 8.75, true};
  static constexpr SplineAxis torusV{-4.125, -1.125, true};

  std::string surface(const SplineAxis &u, const SplineAxis &v,
                      const std::vector<std::vector<std::string>> &control) {
    std::vector<std::string> rows, weightRows;
    for (size_t i = 0; i < u.controls(); ++i) {
      std::vector<std::string> weights;
      for (size_t j = 0; j < v.controls(); ++j)
        weights.push_back(number(u.weight(i) * v.weight(j)));
      rows.push_back("(" + refs(control[i]) + ")");
      weightRows.push_back("(" + refs(weights) + ")");
    }
    return add(std::string("IFCRATIONALBSPLINESURFACEWITHKNOTS(") + u.degree() +
               "," + v.degree() + ",(" + refs(rows) + "),.UNSPECIFIED.," +
               u.closed() + "," + v.closed() + ",.F.," + u.multiplicities() +
               "," + v.multiplicities() + "," + u.knots() + "," + v.knots() +
               ",.UNSPECIFIED.,(" + refs(weightRows) + "))");
  }

  std::string ring(const std::vector<std::string> &control,
                   const SplineAxis &axis) {
    std::vector<std::string> weights;
    for (size_t i = 0; i < control.size(); ++i)
      weights.push_back(number(axis.weight(i)));
    return add("IFCRATIONALBSPLINECURVEWITHKNOTS(2,(" + refs(control) +
               "),.UNSPECIFIED.,.T.,.F.," + axis.multiplicities() + "," +
               axis.knots() + ",.UNSPECIFIED.,(" + refs(weights) + "))");
  }

  std::string pcurve(const std::string &basis, glm::dvec2 a, glm::dvec2 b) {
    return pcurve(basis, {a, b});
  }

  std::string pcurve(const std::string &basis,
                     std::initializer_list<glm::dvec2> points) {
    return pcurve(basis, std::span(points.begin(), points.size()));
  }

  std::string pcurve(const std::string &basis,
                     std::span<const glm::dvec2> points) {
    std::vector<std::string> controls;
    for (const auto p : points)
      controls.push_back(
          add("IFCCARTESIANPOINT((" + number(p.x) + "," + number(p.y) + "))"));
    const auto line = add("IFCPOLYLINE((" + refs(controls) + "))");
    const auto definition =
        add("IFCDEFINITIONALREPRESENTATION($,$,'Curve2D',(" + line + "))");
    return add("IFCPCURVE(" + basis + "," + definition + ")");
  }

  std::string associated(std::string_view type, const std::string &curve,
                         const std::vector<std::string> &lifts,
                         std::string_view master = ".CURVE3D.") {
    return add(std::string(type) + "(" + curve + ",(" + refs(lifts) + ")," +
               std::string(master) + ")");
  }

  std::string cylinder(bool swapped, SplineLift lift,
                       DetachedSplineTrim detached = DetachedSplineTrim::None) {
    const SplineAxis angle{2.75, 8.75, true}, height{-2.125, 4.875, false};
    std::vector<std::vector<std::string>> grid(swapped ? height.controls()
                                                       : angle.controls());
    std::vector<std::string> lower, upper;
    for (const auto &control : circleControls) {
      const auto low = point({2 * control.x, 2 * control.y, 0});
      const auto high = point({2 * control.x, 2 * control.y, 3});
      lower.push_back(low);
      upper.push_back(high);
      if (swapped) {
        grid[0].push_back(low);
        grid[1].push_back(high);
      } else
        grid[lower.size() - 1] = {low, high};
    }
    const auto sheet =
        swapped ? surface(height, angle, grid) : surface(angle, height, grid);
    const auto liftAt = [&](glm::dvec2 a, glm::dvec2 b) {
      return pcurve(sheet, swapped ? glm::dvec2(a.y, a.x) : a,
                    swapped ? glm::dvec2(b.y, b.x) : b);
    };
    const auto ringEdge = [&](const std::vector<std::string> &controls,
                              double z, double shift, bool ambiguous,
                              bool bare) {
      const auto curve = ring(controls, angle);
      if (bare)
        return edge(controls.front(), controls.front(), curve);
      std::vector lifts{
          liftAt({angle.low + shift, z}, {angle.high + shift, z})};
      if (ambiguous)
        lifts.push_back(
            liftAt({angle.high, z}, {2 * angle.high - angle.low, z}));
      return edge(controls.front(), controls.front(),
                  associated("IFCSURFACECURVE", curve, lifts));
    };
    const bool outerBare = detached == DetachedSplineTrim::BareOuter;
    const bool bareBottom = outerBare || lift == SplineLift::MixedRings ||
                            lift == SplineLift::MixedConflict;
    const bool bareSeam = outerBare || lift == SplineLift::MixedSeam;
    const double period = angle.high - angle.low;
    const bool hasHole = detached != DetachedSplineTrim::None;
    const double seamShift = hasHole                               ? 2 * period
                             : (bareBottom || bareSeam) && swapped ? period
                                                                   : 0.;
    const auto bottom =
        ringEdge(lower, height.low, seamShift, false, bareBottom);
    const double topShift =
        seamShift + (lift == SplineLift::WrongRing       ? period / 2
                     : lift == SplineLift::MixedConflict ? period
                                                         : 0.);
    const auto top = ringEdge(upper, height.high, topShift,
                              lift == SplineLift::AmbiguousRing,
                              outerBare || lift == SplineLift::MixedRings);
    const double first = angle.low + seamShift;
    const double second =
        seamShift + (lift == SplineLift::DuplicateSeam ? angle.low
                     : lift == SplineLift::TwiceSeam   ? angle.high + period
                                                       : angle.high);
    const auto seam3d =
        add("IFCPOLYLINE((" + lower.front() + "," + upper.front() + "))");
    const auto seamGeometry =
        bareSeam
            ? seam3d
            : associated("IFCSEAMCURVE", seam3d,
                         {liftAt({first, height.low}, {first, height.high}),
                          liftAt({second, height.low}, {second, height.high})});
    const auto seam = edge(lower.front(), upper.front(), seamGeometry);
    std::vector bounds{
        bound({{bottom, true}, {seam, true}, {top, false}, {seam, false}}, true,
              bareBottom || !swapped)};
    std::vector<Use> patch;
    if (hasHole) {
      // The patch spans one exact rational quadrant and half of the height.
      // Give the wall and patch opposite uses of the same four native edges.
      const unsigned start = detached == DetachedSplineTrim::HoleAtSeam ? 0 : 2;
      const double u0 = angle.low + period * start / 8, u1 = u0 + period / 4;
      const double v0 = std::lerp(height.low, height.high, .25),
                   v1 = std::lerp(height.low, height.high, .75);
      const auto controls = [&](double z) {
        std::array<std::string, 3> refs;
        for (unsigned j = 0; j < refs.size(); ++j) {
          const auto p = circleControls[start + j];
          refs[j] = point({2 * p.x, 2 * p.y, z});
        }
        return refs;
      };
      const auto low = controls(.75), high = controls(2.25);
      const auto patchEdge = [&](const std::string &a, const std::string &b,
                                 const std::string &curve, glm::dvec2 p,
                                 glm::dvec2 q) {
        p.x += seamShift;
        q.x += seamShift;
        return edge(a, b,
                    outerBare
                        ? associated("IFCSURFACECURVE", curve, {liftAt(p, q)})
                        : curve);
      };
      const auto arc = [&](const auto &p, double v) {
        const auto curve =
            add("IFCRATIONALBSPLINECURVEWITHKNOTS(2,(" + p[0] + "," + p[1] +
                "," + p[2] + "),.UNSPECIFIED.,.F.,.F.,(3,3),(" + number(u0) +
                "," + number(u1) + "),.UNSPECIFIED.,(1.," +
                number(angle.weight(1)) + ",1.))");
        return patchEdge(p[0], p[2], curve, {u0, v}, {u1, v});
      };
      const auto side = [&](unsigned i, double u) {
        const auto curve = add("IFCPOLYLINE((" + low[i] + "," + high[i] + "))");
        return patchEdge(low[i], high[i], curve, {u, v0}, {u, v1});
      };
      patch = {{arc(low, v0), true},
               {side(2, u1), true},
               {arc(high, v1), false},
               {side(0, u0), false}};
      auto hole = patch;
      std::ranges::reverse(hole);
      for (auto &use : hole)
        use.second = !use.second;
      bounds.push_back(bound(std::move(hole), false));
    }
    face(bounds, sheet, !swapped);
    if (hasHole) {
      face({bound(std::move(patch))}, sheet, !swapped);
      const auto color = add("IFCCOLOURRGB($,1.,0.,0.)");
      const auto shading = add("IFCSURFACESTYLESHADING(" + color + ",0.)");
      const auto style =
          add("IFCSURFACESTYLE('Trim patch',.BOTH.,(" + shading + "))");
      add("IFCSTYLEDITEM(" + faces.back() + ",(" + style + "),$)");
    }
    plane({{bottom, false}}, {0, 0, 0}, {0, 0, -1});
    plane({{top, true}}, {0, 0, 3}, {0, 0, 1});
    return finish();
  }

  std::vector<std::vector<std::string>> torusGrid() {
    std::vector<std::vector<std::string>> grid;
    for (const auto &around : circleControls) {
      std::vector<std::string> meridian;
      for (const auto &across : circleControls) {
        const double radius = 3 + across.x;
        meridian.push_back(
            point({radius * around.x, radius * around.y, across.y}));
      }
      grid.push_back(std::move(meridian));
    }
    return grid;
  }

  std::string torus() {
    const auto u = torusU, v = torusV;
    const auto grid = torusGrid();
    std::vector<std::string> outerRing;
    for (const auto &meridian : grid)
      outerRing.push_back(meridian.front());
    const auto sheet = surface(u, v, grid);
    const auto corner = outerRing.front();
    const auto seam = [&](const std::string &curve, glm::dvec2 a0,
                          glm::dvec2 b0, glm::dvec2 a1, glm::dvec2 b1) {
      return edge(corner, corner,
                  associated("IFCSEAMCURVE", curve,
                             {pcurve(sheet, a0, b0), pcurve(sheet, a1, b1)}));
    };
    const glm::dvec2 a{u.low, v.low}, b{u.high, v.low}, c{u.high, v.high},
        d{u.low, v.high};
    const auto ringSeam = seam(ring(outerRing, u), a, b, d, c);
    const auto meridianSeam = seam(ring(grid.front(), v), a, d, b, c);
    face({bound({{ringSeam, true},
                 {meridianSeam, true},
                 {ringSeam, false},
                 {meridianSeam, false}})},
         sheet);
    return finish();
  }

  // One U quadrant and half a V quadrant in each of eight Bezier spans.
  // Multiplying their homogeneous quadratic polynomials yields an exact
  // quartic witness for the linear (2,1) lift through the tensor surface.
  std::pair<std::string, std::string> torusHelix(unsigned vQuarterOffset) {
    const auto quadrant = [](unsigned quarter) {
      std::array<glm::dvec3, 3> h;
      for (unsigned i = 0; i < h.size(); ++i) {
        const double w = torusU.weight(i);
        h[i] = {w * circleControls[2 * (quarter % 4) + i], w};
      }
      return h;
    };
    constexpr std::array<double, 3> quadratic{1, 2, 1};
    constexpr std::array<double, 5> quartic{1, 4, 6, 4, 1};
    std::vector<std::string> controls, weights, knots;
    for (unsigned span = 0; span < 8; ++span) {
      auto v = quadrant(span / 2 + vQuarterOffset);
      const auto left = (v[0] + v[1]) / 2.;
      const auto right = (v[1] + v[2]) / 2.;
      const auto middle = (left + right) / 2.;
      v = span % 2 == 0 ? std::array{v[0], left, middle}
                        : std::array{middle, right, v[2]};
      const auto u = quadrant(span);
      std::array<glm::dvec4, 5> product;
      product.fill(glm::dvec4(0.));
      for (unsigned i = 0; i < u.size(); ++i)
        for (unsigned j = 0; j < v.size(); ++j) {
          const auto a = u[i], b = v[j];
          const double r = 3. * b.z + b.x;
          product[i + j] += glm::dvec4(r * a.x, r * a.y, a.z * b.y, a.z * b.z) *
                            (quadratic[i] * quadratic[j] / quartic[i + j]);
        }
      for (unsigned i = span == 0 ? 0 : 1; i < product.size(); ++i) {
        const auto h = product[i];
        controls.push_back(point(glm::dvec3(h) / h.w));
        weights.push_back(number(h.w));
      }
      knots.push_back(number(span + 1));
    }
    knots.push_back("9.");
    return {controls.front(),
            add("IFCRATIONALBSPLINECURVEWITHKNOTS(4,(" + refs(controls) +
                "),.UNSPECIFIED.,.T.,.F.,(5,4,4,4,4,4,4,4,5),(" + refs(knots) +
                "),.UNSPECIFIED.,(" + refs(weights) + "))")};
  }

  std::string torusBands(bool coincident = false) {
    const auto grid = torusGrid();
    const auto lower = surface(torusU, torusV, grid);
    const auto upper = surface(torusU, torusV, grid);
    const double turn = torusV.high - torusV.low;
    const glm::dvec2 winding{2 * (torusU.high - torusU.low), turn};
    const auto boundary = [&](unsigned quarter, glm::dvec2 shift) {
      const auto [corner, curve] = torusHelix(quarter);
      const glm::dvec2 start{torusU.low, torusV.low + quarter * turn / 4};
      const glm::dvec2 translated = start + shift;
      return edge(corner, corner,
                  associated("IFCSURFACECURVE", curve,
                             {pcurve(lower, start, start + winding),
                              pcurve(upper, translated, translated + winding)},
                             ".PCURVE_S1."));
    };
    // Shifting A back one U period advances half a V period at a fixed U.
    // B lies midway between those adjacent lifts of A.
    const auto a = boundary(0, {torusU.low - torusU.high, 0.});
    const auto b = boundary(coincident ? 2 : 1, {0., 0.});
    face({bound({{a, true}}, false), bound({{b, false}}, false)}, lower);
    face({bound({{b, true}}, false), bound({{a, false}}, false)}, upper);
    return finish();
  }

  std::pair<std::string, std::string>
  torusOrthogonalRail(std::span<const glm::dvec2> points) {
    // Restrict a homogeneous quadratic circle quadrant to [first,last].
    // The endpoint and its first derivative fix the restricted control.
    const auto circle = [](double first, double last) {
      const int span = int(std::floor(2 * (first + last)));
      const size_t quadrant = size_t(span % 4 + 4) % 4;
      const double a = 4 * first - span, b = 4 * last - span;
      std::array<glm::dvec3, 3> h;
      for (size_t i = 0; i < h.size(); ++i) {
        const double w = torusU.weight(i);
        h[i] = {circleControls[2 * quadrant + i] * w, w};
      }
      const auto at = [&](double t) {
        return (1 - t) * (1 - t) * h[0] + 2 * t * (1 - t) * h[1] + t * t * h[2];
      };
      const auto start = at(a);
      const auto halfDerivative = (1 - a) * (h[1] - h[0]) + a * (h[2] - h[1]);
      return std::array{start, start + (b - a) * halfDerivative, at(b)};
    };

    std::vector<std::string> controls, weights, knots{"0"}, multiplicities{"3"};
    double joinWeight = 1.;
    for (size_t i = 1; i < points.size(); ++i) {
      const auto a = points[i - 1], b = points[i];
      const int axis = a.x == b.x ? 1 : 0;
      const auto fixed = circle(a[1 - axis], a[1 - axis]).front();
      const auto fixedDirection = glm::dvec2(fixed) / fixed.z;
      const auto [lo, hi] = std::minmax(a[axis], b[axis]);
      std::vector<double> cuts{lo};
      for (double q = std::floor(4 * lo) + 1; q < 4 * hi; ++q)
        cuts.push_back(q / 4);
      cuts.push_back(hi);
      if (a[axis] > b[axis])
        std::ranges::reverse(cuts);
      for (size_t j = 1; j < cuts.size(); ++j) {
        const auto arc = circle(cuts[j - 1], cuts[j]);
        // Uniform scaling preserves each arc and shares its first
        // homogeneous control with the preceding arc's last control.
        const double gauge = joinWeight / arc.front().z;
        for (size_t k = controls.empty() ? 0 : 1; k < arc.size(); ++k) {
          const auto direction = glm::dvec2(arc[k]) / arc[k].z;
          const auto u = axis == 0 ? direction : fixedDirection;
          const auto v = axis == 1 ? direction : fixedDirection;
          controls.push_back(point({(3 + v.x) * u.x, (3 + v.x) * u.y, v.y}));
          weights.push_back(number(arc[k].z * gauge));
        }
        joinWeight = arc.back().z * gauge;
        knots.push_back(std::to_string(knots.size()));
        multiplicities.emplace_back("2");
      }
    }
    multiplicities.back() = "3";
    return {controls.front(),
            add("IFCRATIONALBSPLINECURVEWITHKNOTS(2,(" + refs(controls) +
                "),.UNSPECIFIED.,.T.,.F.,(" + refs(multiplicities) + "),(" +
                refs(knots) + "),.UNSPECIFIED.,(" + refs(weights) + "))")};
  }

  std::string torusBacktrackingBands(bool intersectsImage) {
    const auto grid = torusGrid();
    const auto lower = surface(torusU, torusV, grid);
    const auto upper = surface(torusU, torusV, grid);
    constexpr std::array<glm::dvec2, 8> rail{{{0., .125},
                                              {.375, .125},
                                              {.375, .25},
                                              {.125, .25},
                                              {.125, .375},
                                              {.625, .375},
                                              {.625, .125},
                                              {1., .125}}};
    const auto nativeRail = [&](glm::dvec2 shift) {
      auto result = rail;
      for (auto &uv : result) {
        uv += shift;
        uv = {torusU.low + uv.x * (torusU.high - torusU.low),
              torusV.low + uv.y * (torusV.high - torusV.low)};
      }
      return result;
    };
    const auto boundary = [&](glm::dvec2 shift, glm::dvec2 upperLift) {
      auto geometry = rail;
      for (auto &uv : geometry)
        uv += shift;
      const auto [corner, curve] = torusOrthogonalRail(geometry);
      return edge(corner, corner,
                  associated("IFCSURFACECURVE", curve,
                             {pcurve(lower, nativeRail(shift)),
                              pcurve(upper, nativeRail(shift + upperLift))}));
    };
    // B's translated copy intersects A at (3/8,3/16) only in the bad case.
    const auto a = boundary({0, 0}, {0, 1});
    const auto b = boundary(
        intersectsImage ? glm::dvec2(.0625, .9375) : glm::dvec2(0, .5), {0, 0});
    face({bound({{a, true}}, false), bound({{b, false}}, false)}, lower);
    face({bound({{b, true}}, false), bound({{a, false}}, false)}, upper);
    const auto color = add("IFCCOLOURRGB('Upper band',1.,0.,0.)");
    const auto shading = add("IFCSURFACESTYLESHADING(" + color + ",0.)");
    const auto style =
        add("IFCSURFACESTYLE('Upper band',.BOTH.,(" + shading + "))");
    add("IFCSTYLEDITEM(" + faces.back() + ",(" + style + "),$)");
    return finish();
  }
};

TEST(IfcCurvedBrep, SharpSplineKnotSubdividesStraightSharedEdge) {
  constexpr double crease = .73, slope = .3, height = 2.;
  constexpr float sharedZ = static_cast<float>(crease);
  enum class Trims { Authored, Mixed, Inferred };
  for (const auto trims : {Trims::Authored, Trims::Mixed, Trims::Inferred}) {
    for (bool planarFirst : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "trims=" << static_cast<int>(trims)
                                        << " planarFirst=" << planarFirst);
      PeriodicSplineBuilder step;
      const auto a = step.point({0, 1, 0}), A = step.point({0, 1, height});
      const auto b = step.point({1, 1 + slope * crease, 0}),
                 B = step.point({1, 1 + slope * (height - crease), height}),
                 bend = step.point({1, 1, crease});
      const auto c = step.point({1, 0, 0}), C = step.point({1, 0, height}),
                 d = step.point({0, 0, 0}), D = step.point({0, 0, height});

      // S(u,v) = (1-u, 1 + slope*(1-u)*abs(v-crease), v).
      // The u=1 edge is physically straight and has no authored knot sample.
      const auto sheet = step.add(
          "IFCBSPLINESURFACEWITHKNOTS(1,1,((" + step.refs({b, bend, B}) +
          "),(" + step.refs({a, step.point({0, 1, crease}), A}) +
          ")),.UNSPECIFIED.,.F.,.F.,.F.,(2,2),(2,1,2),(0.,1.),(0.," +
          number(crease) + ",2.),.UNSPECIFIED.)");
      const auto sheetEdge = [&](const std::string &start,
                                 const std::string &end,
                                 std::initializer_list<glm::dvec2> uv) {
        const auto curve =
            step.add("IFCPOLYLINE((" + step.refs({start, end}) + "))");
        if (trims == Trims::Inferred)
          return step.edge(start, end, curve);
        return step.edge(start, end,
                         step.associated("IFCSURFACECURVE", curve,
                                         {step.pcurve(sheet, uv)}));
      };
      const auto bottom = sheetEdge(b, a, {{0, 0}, {1, 0}}),
                 top = sheetEdge(A, B, {{1, height}, {0, height}});
      const auto straight = trims == Trims::Authored
                                ? sheetEdge(a, A, {{1, 0}, {1, height}})
                                : step.line(a, A);
      const auto bentCurve =
          step.add("IFCPOLYLINE((" + step.refs({b, bend, B}) + "))");
      const auto bent = step.edge(
          b, B,
          trims == Trims::Inferred
              ? bentCurve
              : step.associated(
                    "IFCSURFACECURVE", bentCurve,
                    {step.pcurve(sheet, {{0, 0}, {0, crease}, {0, height}})}));
      step.face(
          {step.bound(
              {{bottom, true}, {straight, true}, {top, true}, {bent, false}})},
          sheet);

      const auto bc = step.line(b, c), cd = step.line(c, d),
                 da = step.line(d, a), BC = step.line(B, C),
                 CD = step.line(C, D), DA = step.line(D, A),
                 cC = step.line(c, C), dD = step.line(d, D);
      step.plane({{bottom, false}, {bc, true}, {cd, true}, {da, true}},
                 {0, 0, 0}, {0, 0, -1});
      step.plane({{DA, false}, {CD, false}, {BC, false}, {top, false}},
                 {0, 0, height}, {0, 0, 1});
      step.plane({{straight, false}, {da, false}, {dD, true}, {DA, true}},
                 {0, 0, 0}, {-1, 0, 0});
      step.plane({{bent, true}, {BC, true}, {cC, false}, {bc, false}},
                 {1, 0, 0}, {1, 0, 0});
      step.plane({{cd, false}, {cC, true}, {CD, true}, {dD, false}}, {0, 0, 0},
                 {0, -1, 0});
      if (planarFirst)
        std::ranges::reverse(step.faces);
      const auto brep = step.finish();
      const auto model = LoadFromStep(fixture(step.definitions, brep));
      ASSERT_NO_FATAL_FAILURE(complete(model));
      watertight(model);
      EXPECT_NEAR(volume(model), 2.160935, .002);

      const auto onStraight = [](glm::vec3 p) { return p.x == 0 && p.y == 1; };
      std::array<unsigned, 2> incoming{}, outgoing{};
      for (size_t i = 0; i < model.indices.size(); i += 3) {
        const auto &a = model.vertices.at(model.indices.at(i));
        const auto &b = model.vertices.at(model.indices.at(i + 1));
        const auto &c = model.vertices.at(model.indices.at(i + 2));
        const auto [lo, hi] =
            std::minmax({a.position.z, b.position.z, c.position.z});
        const bool onSheet =
            a.normal.y > .8 && b.normal.y > .8 && c.normal.y > .8;
        if (onSheet)
          EXPECT_FALSE(lo < sharedZ && hi > sharedZ);
        const std::array points{a.position, b.position, c.position};
        for (size_t j = 0; j < points.size(); ++j) {
          const auto p = points[j], q = points[(j + 1) % points.size()];
          if (!onStraight(p) || !onStraight(q))
            continue;
          const auto [first, last] = std::minmax(p.z, q.z);
          EXPECT_FALSE(first < sharedZ && last > sharedZ);
          if (!onSheet)
            EXPECT_TRUE(a.normal.x < -.999 && b.normal.x < -.999 &&
                        c.normal.x < -.999);
          incoming[onSheet] += first < sharedZ && last == sharedZ;
          outgoing[onSheet] += first == sharedZ && last > sharedZ;
        }
      }
      EXPECT_EQ(incoming, (std::array{1u, 1u}));
      EXPECT_EQ(outgoing, (std::array{1u, 1u}));

      std::array<bool, 2> creaseNormals{};
      for (const auto &vertex : model.vertices) {
        if (vertex.position != glm::vec3(1, 1, sharedZ))
          continue;
        for (size_t i = 0; i < creaseNormals.size(); ++i) {
          const auto expected =
              glm::normalize(glm::dvec3(0, 1, i ? slope : -slope));
          creaseNormals[i] |=
              glm::dot(glm::dvec3(vertex.normal), expected) > .999;
        }
      }
      EXPECT_TRUE(creaseNormals[0]);
      EXPECT_TRUE(creaseNormals[1]);
    }
  }
}

template <typename OnSeam>
size_t countSeamEdges(const Model &model, OnSeam onSeam) {
  using Point = std::array<float, 3>;
  std::set<std::pair<Point, Point>> edges;
  for (size_t i = 0; i < model.indices.size(); i += 3)
    for (size_t j = 0; j < 3; ++j) {
      const auto a = model.vertices[model.indices[i + j]].position;
      const auto b = model.vertices[model.indices[i + (j + 1) % 3]].position;
      if (onSeam(a) && onSeam(b)) {
        const Point x{a.x, a.y, a.z}, y{b.x, b.y, b.z};
        edges.insert(std::minmax(x, y));
      }
    }
  return edges.size();
}

void expectPeriodicSplineCylinder(const Model &model) {
  watertight(model);
  EXPECT_NEAR(volume(model), 12 * pi, .045);
  expectProfileBounds(model, {-2, -2, 0}, {2, 2, 3});
  const auto onSeam = [](glm::vec3 p) {
    return std::abs(p.x - 2.) < 1e-6 && std::abs(p.y) < 1e-6;
  };
  EXPECT_GT(countSeamEdges(model, onSeam), 0u);
  size_t seamNormals = 0;
  double agreement = 1.;
  for (const auto &vertex : model.vertices) {
    if (std::abs(vertex.normal.z) > .9)
      continue;
    const glm::dvec2 xy(vertex.position);
    agreement = std::min(
        agreement, glm::dot(glm::normalize(xy), glm::dvec2(vertex.normal)));
    seamNormals += onSeam(vertex.position);
  }
  EXPECT_GT(agreement, .998);
  EXPECT_GE(seamNormals, 2u);
}

TEST(IfcCurvedBrep,
     PeriodicSplineCylinderAcceptsNativePcurveLiftsInEitherAxis) {
  for (bool swapped : {false, true}) {
    SCOPED_TRACE(swapped ? "periodic V" : "periodic U");
    PeriodicSplineBuilder step;
    const auto brep = step.cylinder(swapped, SplineLift::Valid);
    const auto source = fixture(step.definitions, brep);
    exportCurvedFixture(swapped ? "periodic-spline-cylinder-v"
                                : "periodic-spline-cylinder-u",
                        source);
    const auto model = LoadFromStep(source);
    ASSERT_NO_FATAL_FAILURE(complete(model));
    expectPeriodicSplineCylinder(model);
  }
}

TEST(IfcCurvedBrep, MixedSplineCylinderKeepsBareCapRimsAndAuthoredSeams) {
  for (bool swapped : {false, true}) {
    SCOPED_TRACE(swapped ? "periodic V" : "periodic U");
    PeriodicSplineBuilder step;
    const auto brep = step.cylinder(swapped, SplineLift::MixedRings);
    const auto source = fixture(step.definitions, brep);
    exportCurvedFixture(swapped ? "mixed-spline-cylinder-v"
                                : "mixed-spline-cylinder-u",
                        source);
    const auto model = LoadFromStep(source);
    EXPECT_NO_FATAL_FAILURE(complete(model));
    if (!model.indices.empty())
      expectPeriodicSplineCylinder(model);
  }
}

TEST(IfcCurvedBrep, MixedSplineCylinderRejectsIncompatibleRawChartAnchors) {
  for (bool swapped : {false, true}) {
    SCOPED_TRACE(swapped ? "periodic V" : "periodic U");
    PeriodicSplineBuilder step;
    const auto brep = step.cylinder(swapped, SplineLift::MixedConflict);
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.elements.empty());
    EXPECT_TRUE(model.indices.empty());
    EXPECT_TRUE(model.vertices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    const auto &reason = model.importReport.diagnostics.front().reason;
    EXPECT_NE(reason.find("Face #"), std::string::npos) << reason;
    EXPECT_NE(reason.find("Pcurve lifts do not connect in the spline chart"),
              std::string::npos)
        << reason;
  }
}

TEST(IfcCurvedBrep, MixedSplineCylinderInfersOppositeUsesOfOneBareSeam) {
  for (bool swapped : {false, true}) {
    SCOPED_TRACE(swapped ? "periodic V" : "periodic U");
    PeriodicSplineBuilder step;
    const auto brep = step.cylinder(swapped, SplineLift::MixedSeam);
    const auto source = fixture(step.definitions, brep);
    exportCurvedFixture(swapped ? "mixed-spline-bare-seam-v"
                                : "mixed-spline-bare-seam-u",
                        source);
    const auto model = LoadFromStep(source);
    EXPECT_NO_FATAL_FAILURE(complete(model));
    if (!model.indices.empty())
      expectPeriodicSplineCylinder(model);
  }
}

TEST(IfcCurvedBrep, MixedSplineProjectionRejectsASecondNativeRoot) {
  using namespace container::geometry::ifc::detail;
  ParametricSurface surface;
  surface.spline = true;
  surface.uDomain = {{0., 2 * pi}};
  surface.vDomain = {{0., 1.}};
  surface.periods = {2 * pi, 0.};
  surface.point = [](double u, double v) {
    return glm::dvec3(std::cos(2 * u), std::sin(2 * u), v);
  };
  surface.derivatives = [](double u, double) {
    return std::pair(glm::dvec3(-2 * std::sin(2 * u), 2 * std::cos(2 * u), 0),
                     glm::dvec3(0, 0, 1));
  };
  const std::array<glm::vec3, 2> edge{{{1, 0, .25}, {1, 0, .75}}};
  try {
    (void)projectIfcBrepSplineEdge(surface, edge, 1.);
    ADD_FAILURE() << "Projection selected one of two distinct native roots";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(
        std::string_view(error.what()).find("Ambiguous curved face inverse"),
        std::string_view::npos)
        << error.what();
  }
}

TEST(IfcCurvedBrep, DetachedSplineTrimInfersEitherLoopAndKeepsSharedPatch) {
  for (const auto trim :
       {DetachedSplineTrim::BareHole, DetachedSplineTrim::BareOuter})
    for (const bool swapped : {false, true}) {
      SCOPED_TRACE(swapped ? "periodic V" : "periodic U");
      SCOPED_TRACE(trim == DetachedSplineTrim::BareHole ? "bare hole"
                                                        : "bare outer");
      PeriodicSplineBuilder step;
      const auto brep = step.cylinder(swapped, SplineLift::Valid, trim);
      const auto source = fixture(step.definitions, brep);
      if (!swapped)
        exportCurvedFixture(trim == DetachedSplineTrim::BareHole
                                ? "detached-spline-bare-hole"
                                : "detached-spline-bare-outer",
                            source);
      const auto model = LoadFromStep(source);
      EXPECT_NO_FATAL_FAILURE(complete(model));
      if (model.indices.empty())
        continue;
      expectPeriodicSplineCylinder(model);

      const auto patch =
          std::ranges::find_if(model.elements, [](const auto &element) {
            return element.color == glm::vec4(1, 0, 0, 1);
          });
      ASSERT_NE(patch, model.elements.end());
      const auto range =
          std::ranges::find_if(model.meshRanges, [&](const auto &range) {
            return range.meshId == patch->meshId;
          });
      ASSERT_NE(range, model.meshRanges.end());
      double area = 0.;
      const auto position = [&](size_t index) {
        return glm::dvec3(model.vertices.at(model.indices.at(index)).position);
      };
      for (size_t i = range->firstIndex;
           i + 2 < size_t(range->firstIndex) + range->indexCount; i += 3) {
        const auto a = position(i), b = position(i + 1), c = position(i + 2);
        for (const auto p : {a, b, c}) {
          EXPECT_LE(p.x, 1e-6);
          EXPECT_GE(p.y, -1e-6);
          EXPECT_GE(p.z, .75 - 1e-6);
          EXPECT_LE(p.z, 2.25 + 1e-6);
        }
        area += glm::length(glm::cross(b - a, c - a)) / 2.;
      }
      EXPECT_NEAR(area, 1.5 * pi, .02);
    }
}

TEST(IfcCurvedBrep, DetachedSplineTrimRejectsAHoleTouchingTheAuthoredCut) {
  PeriodicSplineBuilder step;
  const auto brep =
      step.cylinder(false, SplineLift::Valid, DetachedSplineTrim::HoleAtSeam);
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(model.elements.empty());
  EXPECT_TRUE(model.indices.empty());
  EXPECT_TRUE(model.vertices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &reason = model.importReport.diagnostics.front().reason;
  EXPECT_NE(reason.find("Face #"), std::string::npos) << reason;
  EXPECT_NE(reason.find("No valid periodic spline trim placement"),
            std::string::npos)
      << reason;
}

TEST(IfcCurvedBrep, AuthoredSplineChartChecksTranslatedInteriorOverlap) {
  using namespace container::geometry::ifc::detail;
  ParametricSurface surface;
  surface.spline = true;
  surface.uDomain = {{0., 2 * pi}};
  surface.vDomain = {{0., 4.}};
  surface.periods = {2 * pi, 0.};
  surface.point = [](double u, double v) -> std::optional<glm::dvec3> {
    return glm::dvec3(std::cos(u), std::sin(u), v);
  };
  surface.derivatives =
      [](double u, double) -> std::optional<std::pair<glm::dvec3, glm::dvec3>> {
    return std::pair(glm::dvec3(-std::sin(u), std::cos(u), 0),
                     glm::dvec3(0, 0, 1));
  };
  for (const double slope : {0., .25}) {
    constexpr unsigned steps = 128;
    const double end = 2 * pi + .5;
    CurvedFaceChartLoop chart;
    for (unsigned i = 0; i <= steps; ++i) {
      const double u = end * i / steps;
      chart.vertices.emplace_back(u, slope * u);
    }
    for (unsigned i = 0; i <= steps; ++i) {
      const double u = end * (steps - i) / steps;
      chart.vertices.emplace_back(u, slope * u + .7);
    }
    chart.closure = chart.vertices.front();
    auto boundaries = std::vector<std::vector<glm::vec3>>(1);
    for (const auto &uv : chart.vertices)
      boundaries.front().emplace_back(*surface.point(uv.x, uv.y));
    std::string error;
    const auto mesh = meshIfcCurvedFace(surface, boundaries, true, true, 1,
                                        error, {}, std::span(&chart, 1));
    if (slope > 0) {
      ASSERT_TRUE(mesh) << error;
      EXPECT_FALSE(mesh->empty());
    } else {
      EXPECT_FALSE(mesh);
      EXPECT_EQ(error, "Spline chart overlaps a periodic image");
    }
  }
}

TEST(IfcCurvedBrep, DoublyPeriodicSplineTorusPairsBothAuthoredSeams) {
  PeriodicSplineBuilder step;
  const auto brep = step.torus();
  const auto source = fixture(step.definitions, brep);
  exportCurvedFixture("periodic-spline-torus", source);
  const auto model = LoadFromStep(source);
  ASSERT_NO_FATAL_FAILURE(complete(model));
  watertight(model);
  EXPECT_NEAR(volume(model), 6 * pi * pi, .15);
  expectProfileBounds(model, {-4, -4, -1}, {4, 4, 1});
  EXPECT_GT(countSeamEdges(model,
                           [](glm::vec3 p) {
                             return std::abs(p.z) < 1e-6 &&
                                    std::abs(std::hypot(p.x, p.y) - 4.) < 1e-6;
                           }),
            32u);
  EXPECT_GT(
      countSeamEdges(
          model, [](glm::vec3 p) { return std::abs(p.y) < 1e-6 && p.x > 0.; }),
      16u);
  double agreement = 1., radialError = 0.;
  for (const auto &vertex : model.vertices) {
    const glm::dvec3 p(vertex.position);
    const glm::dvec3 center = 3. * glm::normalize(glm::dvec3(p.x, p.y, 0));
    const auto outward = p - center;
    radialError = std::max(radialError, std::abs(glm::length(outward) - 1.));
    agreement = std::min(agreement, glm::dot(glm::normalize(outward),
                                             glm::dvec3(vertex.normal)));
  }
  EXPECT_LT(radialError, 1e-5);
  EXPECT_GT(agreement, .998);
}

TEST(IfcCurvedBrep, DoublyPeriodicSplineTorusPairsOppositeMixedWindingBands) {
  PeriodicSplineBuilder step;
  const auto brep = step.torusBands();
  const auto source = fixture(step.definitions, brep);
  exportCurvedFixture("periodic-spline-torus-mixed-bands", source);
  const auto model = LoadFromStep(source);
  ASSERT_NO_FATAL_FAILURE(complete(model));
  watertight(model);
  EXPECT_NEAR(volume(model), 6 * pi * pi, .15);
  double radialError = 0.;
  for (const auto &vertex : model.vertices) {
    const auto &p = vertex.position;
    radialError =
        std::max(radialError,
                 std::abs(std::hypot(std::hypot(double(p.x), double(p.y)) - 3.,
                                     double(p.z)) -
                          1.));
  }
  EXPECT_LT(radialError, 1e-5);
}

TEST(IfcCurvedBrep,
     PeriodicSplineRejectsCoincidentMixedWindingBandsAtomically) {
  PeriodicSplineBuilder step;
  const auto brep = step.torusBands(true);
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(model.elements.empty());
  EXPECT_TRUE(model.indices.empty());
  EXPECT_TRUE(model.vertices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &reason = model.importReport.diagnostics.front().reason;
  EXPECT_NE(reason.find("bounds intersect a periodic image"), std::string::npos)
      << reason;
}

TEST(IfcCurvedBrep, DoublyPeriodicSplineTorusPairsBacktrackingBands) {
  PeriodicSplineBuilder step;
  const auto brep = step.torusBacktrackingBands(false);
  const auto source = fixture(step.definitions, brep);
  exportCurvedFixture("periodic-spline-torus-backtracking-bands", source);
  const auto model = LoadFromStep(source);
  ASSERT_NO_FATAL_FAILURE(complete(model));
  watertight(model);
  EXPECT_NEAR(volume(model), 6 * pi * pi, .15);

  // The style separates the two incident faces into different mesh ranges.
  // Weld by exact positions and compare their independent boundary chains.
  using Point = std::array<float, 3>;
  using Edge = std::pair<Point, Point>;
  ASSERT_EQ(model.meshRanges.size(), 2u);
  std::array<std::set<Edge>, 2> boundaries;
  for (size_t face = 0; face < boundaries.size(); ++face) {
    const auto &range = model.meshRanges[face];
    auto &chain = boundaries[face];
    for (size_t i = range.firstIndex;
         i + 2 < size_t(range.firstIndex) + range.indexCount; i += 3) {
      std::array<Point, 3> points;
      for (size_t j = 0; j < points.size(); ++j) {
        const auto p = model.vertices.at(model.indices.at(i + j)).position;
        points[j] = {p.x, p.y, p.z};
      }
      for (size_t j = 0; j < points.size(); ++j) {
        const Edge edge =
            std::minmax(points[j], points[(j + 1) % points.size()]);
        if (!chain.insert(edge).second)
          chain.erase(edge);
      }
    }
  }
  EXPECT_GT(boundaries[0].size(), 64u);
  EXPECT_TRUE(boundaries[0] == boundaries[1]);
  const float diagonal = static_cast<float>(3 * std::sqrt(.5));
  for (float x : {-diagonal, diagonal}) {
    const Point corner{x, diagonal, 1.f};
    EXPECT_TRUE(std::ranges::any_of(boundaries[0], [&](const Edge &e) {
      return e.first == corner || e.second == corner;
    }));
  }

  double agreement = 1., radialError = 0.;
  for (const auto &vertex : model.vertices) {
    const glm::dvec3 p(vertex.position);
    const auto center = 3. * glm::normalize(glm::dvec3(p.x, p.y, 0));
    const auto outward = p - center;
    radialError = std::max(radialError, std::abs(glm::length(outward) - 1.));
    agreement = std::min(agreement, glm::dot(glm::normalize(outward),
                                             glm::dvec3(vertex.normal)));
  }
  EXPECT_LT(radialError, 1e-5);
  EXPECT_GT(agreement, .998);
}

TEST(IfcCurvedBrep, PeriodicSplineRejectsIntersectingBacktrackingBandImages) {
  PeriodicSplineBuilder step;
  const auto brep = step.torusBacktrackingBands(true);
  const auto model = LoadFromStep(fixture(step.definitions, brep));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_TRUE(model.elements.empty());
  EXPECT_TRUE(model.indices.empty());
  EXPECT_TRUE(model.vertices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &reason = model.importReport.diagnostics.front().reason;
  EXPECT_NE(reason.find("intersect"), std::string::npos) << reason;
  EXPECT_NE(reason.find("periodic image"), std::string::npos) << reason;
}

TEST(IfcCurvedBrep, PeriodicSplineRejectsWrongAmbiguousAndMultipleLifts) {
  for (const auto lift : {SplineLift::WrongRing, SplineLift::AmbiguousRing,
                          SplineLift::DuplicateSeam, SplineLift::TwiceSeam}) {
    SCOPED_TRACE(static_cast<int>(lift));
    PeriodicSplineBuilder step;
    const auto brep = step.cylinder(false, lift);
    const auto model = LoadFromStep(fixture(step.definitions, brep));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.elements.empty());
    EXPECT_TRUE(model.indices.empty());
    EXPECT_TRUE(model.vertices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    const auto &reason = model.importReport.diagnostics.front().reason;
    EXPECT_NE(reason.find("Face #"), std::string::npos) << reason;
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
