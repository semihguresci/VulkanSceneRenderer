#include "../../src/geometry/IfcCurveReader.h"
#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/ParametricCurve.h"
#include "Container/geometry/SweptDiskGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <sstream>

namespace {
using container::geometry::ImportCompleteness;
using container::geometry::ifc::Model;
constexpr double pi = std::numbers::pi;

std::string fixture(std::string_view definitions,
                    std::string_view items = "#10",
                    std::string_view representation = "'Axis','Curve3D'") {
  return "ISO-10303-21; DATA; #1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);" +
         std::string(definitions) + "#900001=IFCSHAPEREPRESENTATION($," +
         std::string(representation) + ",(" + std::string(items) +
         "));"
         "#900002=IFCPRODUCTDEFINITIONSHAPE($,$,(#900001));"
         "#900003=IFCBUILDINGELEMENTPROXY('curve-families',$,'Curves',$,$,$,#"
         "900002,$,$); ENDSEC; END-ISO-10303-21;";
}
std::vector<glm::dvec3> vertices(const Model &model) {
  std::vector<glm::dvec3> r;
  for (const auto &range : model.nativeCurveRanges)
    for (size_t i = range.firstIndex;
         i < size_t(range.firstIndex) + range.indexCount; ++i)
      r.emplace_back(model.vertices.at(model.indices.at(i)).position);
  return r;
}
void complete(const Model &model) {
  for (const auto &d : model.importReport.diagnostics)
    ADD_FAILURE() << d.entityId << ": " << d.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  ASSERT_FALSE(model.nativeCurveRanges.empty());
  for (auto i : model.indices)
    ASSERT_LT(i, model.vertices.size());
}
double length(const std::vector<glm::dvec3> &p) {
  double r = 0;
  for (size_t i = 0; i < p.size(); i += 2)
    r += glm::length(p[i + 1] - p[i]);
  return r;
}
const std::string placement2D =
    "#2=IFCCARTESIANPOINT((0.,0.)); #3=IFCAXIS2PLACEMENT2D(#2,$);";

TEST(IfcCurveFamilies, ConicsPreserveThreeDimensionalPlaneAndClosure) {
  for (bool ellipse : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        "#2=IFCCARTESIANPOINT((1.,2.,3.)); #3=IFCDIRECTION((0.,1.,0.)); "
        "#4=IFCDIRECTION((1.,0.,0.)); #5=IFCAXIS2PLACEMENT3D(#2,#3,#4); #10=" +
        std::string(ellipse ? "IFCELLIPSE(#5,2.,1.);" : "IFCCIRCLE(#5,2.);")));
    complete(model);
    const auto p = vertices(model);
    ASSERT_GT(p.size(), 32u);
    EXPECT_EQ(p.front(), p.back());
    EXPECT_EQ(p.front(), glm::dvec3(3, 2, 3));
    for (auto v : p) {
      EXPECT_NEAR(v.y, 2, 1e-6);
      EXPECT_NEAR((v.x - 1) * (v.x - 1) / 4 +
                      (v.z - 3) * (v.z - 3) / (ellipse ? 1 : 4),
                  1, 1e-6);
    }
    if (!ellipse)
      EXPECT_NEAR(length(p), 4 * pi, .005);
  }
}

TEST(IfcCurveFamilies,
     RationalSplinesFollowCircleAndUniformSplinesKeepTheirDomain) {
  const std::string points =
      "#11=IFCCARTESIANPOINT((1.,0.)); #12=IFCCARTESIANPOINT((1.,1.)); "
      "#13=IFCCARTESIANPOINT((0.,1.));";
  const auto rational = container::geometry::ifc::LoadFromStep(fixture(
      points +
      "#10=IFCRATIONALBSPLINECURVEWITHKNOTS(2,(#11,#12,#13),.UNSPECIFIED.,.F.,."
      "F.,(3,3),(2.,4.),.UNSPECIFIED.,(1.,0.7071067811865476,1.));"));
  complete(rational);
  const auto p = vertices(rational);
  ASSERT_GT(p.size(), 4u);
  EXPECT_EQ(p.front(), glm::dvec3(1, 0, 0));
  EXPECT_EQ(p.back(), glm::dvec3(0, 1, 0));
  for (auto v : p)
    EXPECT_NEAR(glm::length(v), 1, 1e-6);
  EXPECT_NEAR(length(p), pi * .5, .001);
  const auto uniform = container::geometry::ifc::LoadFromStep(fixture(
      "#11=IFCCARTESIANPOINT((0.,0.)); #12=IFCCARTESIANPOINT((1.,2.)); "
      "#13=IFCCARTESIANPOINT((3.,1.)); #14=IFCCARTESIANPOINT((5.,3.));"
      "#10=IFCBSPLINECURVEWITHKNOTS(2,(#11,#12,#13,#14),.UNSPECIFIED.,.F.,.F.,("
      "1,1,1,1,1,1,1),(0.,1.,2.,3.,4.,5.,6.),.UNIFORM_KNOTS.);"));
  complete(uniform);
  const auto u = vertices(uniform);
  ASSERT_FALSE(u.empty());
  EXPECT_EQ(u.front(), glm::dvec3(.5, 1, 0));
  EXPECT_EQ(u.back(), glm::dvec3(4, 2, 0));
}

TEST(IfcCurveFamilies, CartesianAndParameterTrimsRespectSenseAndPeriodicSeam) {
  for (bool cartesian : {false, true}) {
    const std::string base =
        placement2D + "#6=IFCCIRCLE(#3,2.); #7=IFCCARTESIANPOINT((0.,2.)); "
                      "#8=IFCCARTESIANPOINT((2.,0.));";
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        base +
        (cartesian
             ? "#10=IFCTRIMMEDCURVE(#6,(#7,IFCPARAMETERVALUE(1."
               "5707963267948966)),(#8,IFCPARAMETERVALUE(0.)),.F.,.CARTESIAN.);"
             : "#10=IFCTRIMMEDCURVE(#6,(IFCPARAMETERVALUE(4.71238898038469)),("
               "IFCPARAMETERVALUE(1.5707963267948966)),.T.,.PARAMETER.);")));
    complete(model);
    const auto p = vertices(model);
    ASSERT_FALSE(p.empty());
    EXPECT_NEAR(length(p), cartesian ? pi : 2 * pi, .003);
    for (auto v : p)
      EXPECT_GE(v.x, -1e-6);
    EXPECT_NEAR(p.front().y, cartesian ? 2 : -2, 1e-6);
    EXPECT_NEAR(p.back().x, cartesian ? 2 : 0, 1e-6);
  }
  const auto spline = container::geometry::ifc::LoadFromStep(fixture(
      "#11=IFCCARTESIANPOINT((0.,0.)); #12=IFCCARTESIANPOINT((1.,2.)); "
      "#13=IFCCARTESIANPOINT((2.,0.));"
      "#6=IFCBSPLINECURVEWITHKNOTS(2,(#11,#12,#13),.UNSPECIFIED.,.F.,.F.,(3,3),"
      "(0.,1.),.UNSPECIFIED.);"
      "#7=IFCCARTESIANPOINT((0.5,0.75)); #8=IFCCARTESIANPOINT((1.5,0.75));"
      "#10=IFCTRIMMEDCURVE(#6,(#8),(#7),.F.,.CARTESIAN.);"));
  complete(spline);
  const auto p = vertices(spline);
  ASSERT_FALSE(p.empty());
  EXPECT_NEAR(glm::length(p.front() - glm::dvec3(1.5, .75, 0)), 0, 1e-6);
  EXPECT_NEAR(glm::length(p.back() - glm::dvec3(.5, .75, 0)), 0, 1e-6);
}

TEST(IfcCurveFamilies, CompositeReparametrizationAndReversedSegmentsKeepJoins) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.)); #3=IFCCARTESIANPOINT((2.,0.)); "
      "#4=IFCCARTESIANPOINT((2.,3.));"
      "#5=IFCPOLYLINE((#2,#3)); #6=IFCPOLYLINE((#4,#3));"
      "#7=IFCREPARAMETRISEDCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#5,10.);"
      "#8=IFCCOMPOSITECURVESEGMENT(.DISCONTINUOUS.,.F.,#6);"
      "#10=IFCCOMPOSITECURVE((#7,#8),.F.);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_EQ(p.size(), 4u);
  EXPECT_EQ(p[0], glm::dvec3(0));
  EXPECT_EQ(p[1], p[2]);
  EXPECT_EQ(p.back(), glm::dvec3(2, 3, 0));
  EXPECT_NEAR(length(p), 5, 1e-6);
}

TEST(IfcCurveFamilies, ConstantOffsetsFollowIfcLeftHandAndReferenceNormals) {
  for (double offset : {.5, -.5}) {
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(placement2D + "#6=IFCCIRCLE(#3,2.); #10=IFCOFFSETCURVE2D(#6," +
                std::to_string(offset) + ",.F.);"));
    complete(model);
    const auto p = vertices(model);
    ASSERT_FALSE(p.empty());
    for (auto v : p)
      EXPECT_NEAR(glm::length(v), 2 - offset, 1e-6);
    EXPECT_NEAR(length(p), 2 * pi * (2 - offset), .005);
  }
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture("#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCDIRECTION((0.,1.,0.)); "
              "#4=IFCAXIS2PLACEMENT3D(#2,#3,$);"
              "#6=IFCCIRCLE(#4,2.); #10=IFCOFFSETCURVE3D(#6,0.5,.F.,#3);"));
  complete(model);
  for (auto v : vertices(model)) {
    EXPECT_NEAR(v.y, 0, 1e-6);
    EXPECT_NEAR(glm::length(v), 1.5, 1e-6);
  }
}

TEST(IfcCurveFamilies,
     DistanceOffsetsInterpolateInLengthAndExtendEndpointValues) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((10.,0.,0.)); "
      "#4=IFCPOLYLINE((#2,#3));"
      "#5=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(3.),1.,2.,$,#4);"
      "#6=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(7.),3.,4.,$,#4); "
      "#10=IFCOFFSETCURVEBYDISTANCES(#4,(#5,#6),$);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_FALSE(p.empty());
  EXPECT_EQ(p.front(), glm::dvec3(0, 1, 2));
  EXPECT_EQ(p.back(), glm::dvec3(10, 3, 4));
  for (auto v : p) {
    const double f = std::clamp((v.x - 3) / 4, 0., 1.);
    EXPECT_NEAR(v.y, 1 + 2 * f, 1e-6);
    EXPECT_NEAR(v.z, 2 + 2 * f, 1e-6);
  }
}

TEST(IfcCurveFamilies, PcurvesEvaluatePlaneCylinderSphereAndTorus) {
  for (const std::string surface :
       {"IFCPLANE(#4)", "IFCCYLINDRICALSURFACE(#4,2.)",
        "IFCSPHERICALSURFACE(#4,2.)", "IFCTOROIDALSURFACE(#4,2.,0.5)"}) {
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture("#2=IFCCARTESIANPOINT((1.,2.,3.)); "
                "#4=IFCAXIS2PLACEMENT3D(#2,$,$); #5=" +
                surface +
                ";"
                "#6=IFCCARTESIANPOINT((0.,0.)); "
                "#7=IFCCARTESIANPOINT((6.283185307179586,0.));"
                "#8=IFCPOLYLINE((#6,#7)); #10=IFCPCURVE(#5,#8);"));
    complete(model);
    const auto p = vertices(model);
    ASSERT_FALSE(p.empty());
    if (surface.starts_with("IFCPLANE")) {
      EXPECT_NEAR(p.front().x, 1, 1e-6);
      EXPECT_NEAR(p.back().x, 1 + 2 * pi, 1e-6);
    } else
      for (auto v : p) {
        EXPECT_NEAR(v.z, 3, 1e-6);
        EXPECT_NEAR(glm::length(glm::dvec2(v.x - 1, v.y - 2)),
                    surface.starts_with("IFCTOROIDAL") ? 2.5 : 2, 1e-6);
      }
  }
}

TEST(IfcCurveFamilies,
     PcurveEvaluatesRationalSplineSurfaceAndExplicitIntersectionCurve) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#11=IFCCARTESIANPOINT((0.,0.,0.)); #12=IFCCARTESIANPOINT((0.,1.,0.));"
      "#13=IFCCARTESIANPOINT((1.,0.,0.)); #14=IFCCARTESIANPOINT((1.,1.,2.));"
      "#5=IFCRATIONALBSPLINESURFACEWITHKNOTS(1,1,((#11,#12),(#13,#14)),."
      "UNSPECIFIED.,.F.,.F.,.F.,(2,2),(2,2),(0.,1.),(0.,1.),.UNSPECIFIED.,((1.,"
      "1.),(1.,1.)));"
      "#6=IFCCARTESIANPOINT((0.,0.)); #7=IFCCARTESIANPOINT((1.,1.)); "
      "#8=IFCPOLYLINE((#6,#7)); #10=IFCPCURVE(#5,#8);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_GT(p.size(), 4u);
  for (auto v : p) {
    EXPECT_NEAR(v.x, v.y, 1e-6);
    EXPECT_NEAR(v.z, 2 * v.x * v.x, 1e-6);
  }
  const auto intersection = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((1.,0.,0.));"
      "#4=IFCPOLYLINE((#2,#3)); #5=IFCAXIS2PLACEMENT3D(#2,$,$); "
      "#6=IFCPLANE(#5);"
      "#10=IFCINTERSECTIONCURVE(#4,(#6),.CURVE3D.);"));
  complete(intersection);
  EXPECT_EQ(vertices(intersection).size(), 2u);
}

TEST(IfcCurveFamilies, CompositeProfileRetainsNearlyCoincidentSourceJoins) {
  const std::string definitions =
      "#2=IFCCARTESIANPOINT((30.0000000000218,30.0000000000155));"
      "#3=IFCDIRECTION((-1.,-5.15380331004285E-13));"
      "#4=IFCDIRECTION((-9.60187849083997E-14,-1.));"
      "#5=IFCAXIS2PLACEMENT2D(#2,#3); #6=IFCAXIS2PLACEMENT2D(#2,#4);"
      "#7=IFCCIRCLE(#5,30.); #8=IFCCIRCLE(#6,30.);"
      "#20=IFCTRIMMEDCURVE(#7,(IFCPARAMETERVALUE(0.)),(IFCPARAMETERVALUE(1."
      "57079632679365)),.T.,.PARAMETER.);"
      "#21=IFCTRIMMEDCURVE(#8,(IFCPARAMETERVALUE(0.)),(IFCPARAMETERVALUE(1."
      "57079632679947)),.T.,.PARAMETER.);"
      "#30=IFCCARTESIANPOINT((59.9999999997926,29.9999999999309));"
      "#31=IFCCARTESIANPOINT((59.999999999789,44.9999999998727));"
      "#32=IFCCARTESIANPOINT((53.9999999998108,50.9999999998545));"
      "#33=IFCCARTESIANPOINT((53.9999999998054,139.999999999618));"
      "#34=IFCCARTESIANPOINT((5.99999999996726,139.999999999618));"
      "#35=IFCCARTESIANPOINT((5.99999999997453,50.9999999998581));"
      "#36=IFCCARTESIANPOINT((8.36735125631094E-11,44.9999999998799));"
      "#37=IFCCARTESIANPOINT((5.45696821063757E-11,29.9999999999964));"
      "#22=IFCPOLYLINE((#30,#31,#32,#33,#34,#35,#36,#37));"
      "#40=IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#20);"
      "#41=IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#21);"
      "#42=IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#22);"
      "#43=IFCCOMPOSITECURVE((#40,#41,#42),.T.);"
      "#50=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#43);"
      "#10=IFCEXTRUDEDAREASOLID(#50,$,$,30.);";
  const auto curve =
      container::geometry::ifc::LoadFromStep(fixture(definitions, "#43"));
  complete(curve);
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(definitions));
  for (const auto &d : model.importReport.diagnostics)
    ADD_FAILURE() << d.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
}

TEST(IfcCurveFamilies, SurfaceCompositeUsesPreferredPcurveRepresentation) {
  for (const std::string master : {"CURVE3D", "PCURVE_S1", "PCURVE_S2"}) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        "#2=IFCCARTESIANPOINT((0.,0.,4.)); #3=IFCCARTESIANPOINT((1.,0.,4.));"
        "#4=IFCPOLYLINE((#2,#3)); #5=IFCAXIS2PLACEMENT3D(#2,$,$);"
        "#6=IFCPLANE(#5); #7=IFCCARTESIANPOINT((0.,0.));"
        "#8=IFCCARTESIANPOINT((1.,0.)); #9=IFCPOLYLINE((#7,#8));"
        "#20=IFCPCURVE(#6,#9); #21=IFCPCURVE(#6,#9);"
        "#22=IFCSURFACECURVE(#4,(#20,#21),." +
        master +
        ".);"
        "#23=IFCCOMPOSITECURVESEGMENT(.DISCONTINUOUS.,.T.,#22);"
        "#10=IFCCOMPOSITECURVEONSURFACE((#23),.F.);"));
    complete(model);
    const auto p = vertices(model);
    ASSERT_FALSE(p.empty());
    EXPECT_EQ(p.front(), glm::dvec3(0, 0, 4));
    EXPECT_EQ(p.back(), glm::dvec3(1, 0, 4));
    EXPECT_NEAR(length(p), 1, 1e-6);
    if (master == "CURVE3D")
      EXPECT_EQ(p.size(), 2u);
    else
      EXPECT_GT(p.size(), 2u);
  }
}

TEST(IfcCurveFamilies, LineSegmentsUseDistanceBasedLinearPlacementFrames) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      placement2D +
      "#4=IFCDIRECTION((1.,0.)); #5=IFCVECTOR(#4,2.); #6=IFCLINE(#2,#5);"
      "#20=IFCCARTESIANPOINT((0.,0.,0.)); #21=IFCCARTESIANPOINT((10.,0.,0.));"
      "#22=IFCPOLYLINE((#20,#21));"
      "#23=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(2.),1.,2.,3.,#22);"
      "#24=IFCAXIS2PLACEMENTLINEAR(#23,$,$);"
      "#10=IFCCURVESEGMENT(.DISCONTINUOUS.,#24,IFCLENGTHMEASURE(0.),"
      "IFCLENGTHMEASURE(2.),#6);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_EQ(p.size(), 2u);
  EXPECT_EQ(p.front(), glm::dvec3(5, 1, 2));
  EXPECT_EQ(p.back(), glm::dvec3(7, 1, 2));
}

TEST(IfcCurveFamilies, ClosedConicsAndRationalSplinesCanFormExtrudedProfiles) {
  for (bool spline : {false, true}) {
    const std::string outline =
        spline
            ? "#11=IFCCARTESIANPOINT((2.,0.)); #12=IFCCARTESIANPOINT((2.,1.));"
              "#13=IFCCARTESIANPOINT((0.,1.)); #14=IFCCARTESIANPOINT((-2.,1.));"
              "#15=IFCCARTESIANPOINT((-2.,0.)); "
              "#16=IFCCARTESIANPOINT((-2.,-1.));"
              "#17=IFCCARTESIANPOINT((0.,-1.)); "
              "#18=IFCCARTESIANPOINT((2.,-1.));"
              "#6=IFCRATIONALBSPLINECURVEWITHKNOTS(2,(#11,#12,#13,#14,#15,#16,#"
              "17,#18,#11),"
              ".UNSPECIFIED.,.T.,.F.,(3,2,2,2,3),(0.,1.,2.,3.,4.),.UNSPECIFIED."
              ","
              "(1.,0.7071067811865476,1.,0.7071067811865476,1.,0."
              "7071067811865476,1.,0.7071067811865476,1.));"
            : placement2D + "#6=IFCELLIPSE(#3,2.,1.);";
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(outline + "#7=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#6); "
                          "#10=IFCEXTRUDEDAREASOLID(#7,$,$,3.);"));
    ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    ASSERT_FALSE(model.meshRanges.empty());
    double volume = 0;
    for (const auto &r : model.meshRanges)
      for (size_t i = r.firstIndex; i < size_t(r.firstIndex) + r.indexCount;
           i += 3) {
        const glm::dvec3 a = model.vertices[model.indices[i]].position,
                         b = model.vertices[model.indices[i + 1]].position,
                         c = model.vertices[model.indices[i + 2]].position;
        volume += glm::dot(a, glm::cross(b, c)) / 6;
      }
    EXPECT_NEAR(volume, 6 * pi, .03) << spline;
  }
}

// Composite fixtures keep the parent curve unbounded while giving each spiral
// an explicit length and an insertion point with a 90-degree rotation.
TEST(IfcCurveFamilies, EveryIfc43SpiralMatchesIndependentNumericalIntegration) {
  struct Case {
    std::string definition;
    std::function<double(double)> heading;
  };
  const std::vector<Case> cases{
      {"IFCCLOTHOID(#3,20.)", [](double s) { return s * s / 800; }},
      {"IFCCLOTHOID(#3,-20.)", [](double s) { return -s * s / 800; }},
      {"IFCCOSINESPIRAL(#3,20.,100.)",
       [](double s) { return s / 100 + 5 / (pi * 20) * std::sin(pi * s / 5); }},
      {"IFCSINESPIRAL(#3,20.,40.,100.)",
       [](double s) {
         return s / 100 + s * s / (2 * 40 * 40) +
                5 / (2 * pi * 20) * (1 - std::cos(2 * pi * s / 5));
       }},
      {"IFCSECONDORDERPOLYNOMIALSPIRAL(#3,20.,$,$)",
       [](double s) { return s * s * s / (3 * 20 * 20 * 20); }},
      {"IFCTHIRDORDERPOLYNOMIALSPIRAL(#3,20.,$,$,$)",
       [](double s) { return std::pow(s / 20, 4) / 4; }},
      {"IFCSEVENTHORDERPOLYNOMIALSPIRAL(#3,20.,$,$,$,$,$,$,$)",
       [](double s) { return std::pow(s / 20, 8) / 8; }}};
  for (const auto &c : cases) {
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(placement2D +
                "#4=IFCCARTESIANPOINT((10.,5.)); #5=IFCDIRECTION((0.,1.)); "
                "#7=IFCAXIS2PLACEMENT2D(#4,#5); #6=" +
                c.definition +
                ";"
                "#8=IFCCURVESEGMENT(.DISCONTINUOUS.,#7,IFCLENGTHMEASURE(0.),"
                "IFCLENGTHMEASURE(5.),#6); #10=IFCCOMPOSITECURVE((#8),.F.);"));
    complete(model);
    const auto p = vertices(model);
    ASSERT_FALSE(p.empty()) << c.definition;
    glm::dvec3 expected(10, 5, 0);
    constexpr unsigned n = 10000;
    for (unsigned i = 0; i < n; ++i) {
      const double a = c.heading(5 * (i + .5) / n);
      expected += 5. / n * glm::dvec3(-std::sin(a), std::cos(a), 0);
    }
    EXPECT_NEAR(glm::length(p.back() - expected), 0, 2e-6) << c.definition;
    EXPECT_NEAR(length(p), 5, .001) << c.definition;
  }
}

TEST(IfcCurveFamilies,
     PolynomialAndReversedLengthSegmentsRespectInsertionTangent) {
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(placement2D + "#6=IFCPOLYNOMIALCURVE(#3,(0.,1.),(0.,0.,1.),$);"
                            "#10=IFCTRIMMEDCURVE(#6,(IFCPARAMETERVALUE(-1.)),("
                            "IFCPARAMETERVALUE(2.)),.T.,.PARAMETER.);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_FALSE(p.empty());
  EXPECT_EQ(p.front(), glm::dvec3(-1, 1, 0));
  EXPECT_EQ(p.back(), glm::dvec3(2, 4, 0));
  for (auto v : p)
    EXPECT_NEAR(v.y, v.x * v.x, 1e-6);
  const auto reversed = container::geometry::ifc::LoadFromStep(
      fixture(placement2D + "#6=IFCCIRCLE(#3,10.); "
                            "#8=IFCCURVESEGMENT(.DISCONTINUOUS.,#3,"
                            "IFCLENGTHMEASURE(5.),IFCLENGTHMEASURE(-5.),#6);"
                            "#10=IFCCOMPOSITECURVE((#8),.F.);"));
  complete(reversed);
  const auto r = vertices(reversed);
  ASSERT_FALSE(r.empty());
  EXPECT_NEAR(glm::length(r.front()), 0, 1e-6);
  EXPECT_NEAR(r.back().x, 10 * std::sin(.5), 1e-6);
  EXPECT_NEAR(r.back().y, -10 * (1 - std::cos(.5)), 1e-6);
}

const std::string gradientFixture =
    placement2D +
    "#11=IFCCARTESIANPOINT((10.,0.)); #12=IFCPOLYLINE((#2,#11));"
    "#20=IFCPOLYNOMIALCURVE(#3,(0.,1.),(0.,0.,0.01),$);"
    "#21=IFCCURVESEGMENT(.CONTINUOUS.,#3,IFCPARAMETERVALUE(0.),"
    "IFCPARAMETERVALUE(10.),#20);"
    "#22=IFCCARTESIANPOINT((10.,1.)); #23=IFCAXIS2PLACEMENT2D(#22,$);"
    "#24=IFCCURVESEGMENT(.DISCONTINUOUS.,#23,IFCLENGTHMEASURE(0.),"
    "IFCLENGTHMEASURE(0.),#20);"
    "#30=IFCGRADIENTCURVE((#21,#24),.F.,#12,$);";

TEST(IfcCurveFamilies, GradientAndCantKeepBaseParameterAndAddElevation) {
  const auto gradient =
      container::geometry::ifc::LoadFromStep(fixture(gradientFixture, "#30"));
  complete(gradient);
  const auto g = vertices(gradient);
  ASSERT_FALSE(g.empty());
  for (auto p : g) {
    EXPECT_NEAR(p.z, .01 * p.x * p.x, 1e-6);
    EXPECT_NEAR(p.y, 0, 1e-6);
  }
  EXPECT_NEAR(g.back().x, 10, 1e-6);
  const auto cant = container::geometry::ifc::LoadFromStep(fixture(
      gradientFixture +
      "#40=IFCCARTESIANPOINT((0.,0.,0.)); #41=IFCAXIS2PLACEMENT3D(#40,$,$);"
      "#42=IFCCARTESIANPOINT((10.,1.,0.)); "
      "#43=IFCDIRECTION((0.,-0.1,0.99498743710662)); "
      "#44=IFCAXIS2PLACEMENT3D(#42,#43,$);"
      "#45=IFCCLOTHOID(#3,10.); "
      "#46=IFCCURVESEGMENT(.DISCONTINUOUS.,#41,IFCLENGTHMEASURE(0.),"
      "IFCLENGTHMEASURE(10.),#45);"
      "#10=IFCSEGMENTEDREFERENCECURVE((#46),.F.,#30,#44);"));
  complete(cant);
  const auto c = vertices(cant);
  ASSERT_FALSE(c.empty());
  for (auto p : c) {
    EXPECT_NEAR(p.z, .01 * p.x * p.x + .1 * p.x, 1e-6);
    EXPECT_NEAR(p.y, 0, 1e-6);
  }
  EXPECT_NEAR(c.back().z, 2, 1e-6);
}

TEST(IfcCurveFamilies, InvalidFamiliesAreAtomicAndCarryEntityDiagnostics) {
  const std::vector<std::string> invalid{
      placement2D + "#10=IFCCIRCLE(#3,-1.);",
      placement2D + "#6=IFCCIRCLE(#3,1.); "
                    "#10=IFCTRIMMEDCURVE(#6,(IFCPARAMETERVALUE(0.)),("
                    "IFCPARAMETERVALUE(6.283185307179586)),.T.,.PARAMETER.);",
      "#11=IFCCARTESIANPOINT((0.,0.)); #12=IFCCARTESIANPOINT((1.,1.)); "
      "#13=IFCCARTESIANPOINT((2.,0.)); "
      "#10=IFCRATIONALBSPLINECURVEWITHKNOTS(2,(#11,#12,#13),.UNSPECIFIED.,.F.,."
      "F.,(3,3),(0.,1.),.UNSPECIFIED.,(1.,0.,1.));",
      "#11=IFCCARTESIANPOINT((0.,0.)); #12=IFCCARTESIANPOINT((1.,1.)); "
      "#13=IFCCARTESIANPOINT((2.,0.)); "
      "#10=IFCBSPLINECURVEWITHKNOTS(2,(#11,#12,#13),.UNSPECIFIED.,.F.,.F.,(2,2)"
      ",(0.,1.),.UNSPECIFIED.);",
      placement2D + "#10=IFCCLOTHOID(#3,0.);",
      "#10=IFCOFFSETCURVE2D(#10,1.,.F.);",
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((0.,0.,1.)); "
      "#4=IFCPOLYLINE((#2,#3)); #5=IFCDIRECTION((0.,0.,1.)); "
      "#10=IFCOFFSETCURVE3D(#4,1.,.F.,#5);",
      placement2D + "#10=IFCLINE(#2,#999);",
      "#10=IFCGRADIENTCURVE((#999),.F.,#10,$);"};
  for (const auto &definitions : invalid) {
    const auto model =
        container::geometry::ifc::LoadFromStep(fixture(definitions));
    EXPECT_TRUE(model.vertices.empty()) << definitions;
    EXPECT_TRUE(model.indices.empty());
    EXPECT_TRUE(model.nativeCurveRanges.empty());
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics[0].entityId, 10u);
    EXPECT_FALSE(model.importReport.diagnostics[0].reason.empty());
  }
}

TEST(IfcCurveFamilies, PeriodicPcurvesDoNotAliasAwayMultipleTurns) {
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture("#2=IFCCARTESIANPOINT((0.,0.,0.)); "
              "#3=IFCAXIS2PLACEMENT3D(#2,$,$); #4=IFCCYLINDRICALSURFACE(#3,1.);"
              "#5=IFCCARTESIANPOINT((0.,0.)); "
              "#6=IFCCARTESIANPOINT((201.06192982974676,0.)); "
              "#7=IFCPOLYLINE((#5,#6)); #10=IFCPCURVE(#4,#7);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_GT(p.size(), 4096u);
  EXPECT_NEAR(length(p), 64 * pi, .03);
  for (auto v : p)
    EXPECT_NEAR(glm::length(v), 1, 1e-6);
}

TEST(IfcCurveFamilies, NestedParentsHaveBoundedStorageAndDepth) {
  for (unsigned count : {60u, 70u}) {
    std::string definitions =
        "#2=IFCCARTESIANPOINT((0.,0.)); #3=IFCCARTESIANPOINT((1.,0.)); "
        "#4=IFCPOLYLINE((#2,#3));";
    uint32_t parent = 4;
    for (unsigned i = 0; i < count; ++i) {
      const uint32_t id = 100 + i;
      definitions +=
          "#" + std::to_string(id) + "=IFCTRIMMEDCURVE(#" +
          std::to_string(parent) +
          ",(IFCPARAMETERVALUE(0.)),(IFCPARAMETERVALUE(1.)),.T.,.PARAMETER.);";
      parent = id;
    }
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(definitions, "#" + std::to_string(parent)));
    if (count == 60) {
      complete(model);
      EXPECT_EQ(vertices(model).size(), 2u);
    } else {
      EXPECT_TRUE(model.vertices.empty());
      ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
      EXPECT_NE(model.importReport.diagnostics[0].reason.find("depth"),
                std::string::npos);
    }
  }
}

TEST(IfcCurveFamilies, VerticalPolynomialPreservesAuthoredInitialGrade) {
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(placement2D +
              "#11=IFCCARTESIANPOINT((10.,0.)); #12=IFCPOLYLINE((#2,#11));"
              "#20=IFCPOLYNOMIALCURVE(#3,(0.,1.),(0.,-0.1,0.01),$);"
              "#21=IFCCURVESEGMENT(.DISCONTINUOUS.,#3,IFCLENGTHMEASURE(0.),"
              "IFCLENGTHMEASURE(10.),#20);"
              "#10=IFCGRADIENTCURVE((#21),.F.,#12,$);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_FALSE(p.empty());
  EXPECT_NEAR(p.back().x, 10, 1e-6);
  for (auto v : p)
    EXPECT_NEAR(v.z, -.1 * v.x + .01 * v.x * v.x, 1e-6);
}

TEST(IfcCurveFamilies, ParameterTrimsDisambiguateRepeatedCartesianLocations) {
  const std::string base =
      "#11=IFCCARTESIANPOINT((0.,0.)); #12=IFCCARTESIANPOINT((1.,0.));"
      "#6=IFCBSPLINECURVEWITHKNOTS(4,(#11,#12,#11,#11,#12),.UNSPECIFIED.,.F.,."
      "T.,(5,5),(0.,1.),.UNSPECIFIED.);"
      "#7=IFCCARTESIANPOINT((0.4112,0.)); #8=IFCCARTESIANPOINT((1.,0.));";
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(base + "#10=IFCTRIMMEDCURVE(#6,(#7,IFCPARAMETERVALUE(0.2)),(#8),."
                     "T.,.PARAMETER.);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_FALSE(p.empty());
  EXPECT_NEAR(p.front().x, .4112, 1e-6);
  const auto ambiguous = container::geometry::ifc::LoadFromStep(
      fixture(base + "#10=IFCTRIMMEDCURVE(#6,(#7),(#8),.T.,.CARTESIAN.);"));
  EXPECT_TRUE(ambiguous.vertices.empty());
  ASSERT_EQ(ambiguous.importReport.diagnostics.size(), 1u);
  EXPECT_NE(ambiguous.importReport.diagnostics[0].reason.find("ambiguous"),
            std::string::npos);
}

TEST(IfcCurveFamilies, ConicDegreeUnitsAndMappedSourceStyleAreRetained) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      placement2D +
      "#6=IFCCIRCLE(#3,2.); "
      "#7=IFCTRIMMEDCURVE(#6,(IFCPARAMETERVALUE(0.)),(IFCPARAMETERVALUE(90.)),."
      "T.,.PARAMETER.);"
      "#8=IFCSHAPEREPRESENTATION($,'Axis','Curve2D',(#7)); "
      "#9=IFCREPRESENTATIONMAP($,#8);"
      "#10=IFCMAPPEDITEM(#9,$); #11=IFCCOLOURRGB($,1.,0.,0.); "
      "#12=IFCCURVESTYLE($,$,$,#11,$); #13=IFCSTYLEDITEM(#10,(#12),$);"
      "#30=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.); "
      "#31=IFCMEASUREWITHUNIT(IFCPLANEANGLEMEASURE(0.017453292519943295),#30);"
      "#32=IFCCONVERSIONBASEDUNIT($,.PLANEANGLEUNIT.,'degree',#31); "
      "#33=IFCUNITASSIGNMENT((#1,#32));"
      "#34=IFCPROJECT('project',$,$,$,$,$,$,$,#33);"));
  complete(model);
  const auto p = vertices(model);
  ASSERT_FALSE(p.empty());
  EXPECT_NEAR(length(p), pi, .002);
  EXPECT_NEAR(p.back().y, 2, 1e-6);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].color, glm::vec4(1, 0, 0, 1));
}

std::string stylePolyline(double sourceUnitsPerMeter) {
  const auto length = [&](double x) {
    return std::to_string(x * sourceUnitsPerMeter);
  };
  return "#2=IFCCARTESIANPOINT((0.,0.,0.)); "
         "#3=IFCCARTESIANPOINT((" +
         length(3) +
         ",0.,0.)); "
         "#4=IFCCARTESIANPOINT((" +
         length(3) + ",0.," + length(4) +
         ")); "
         "#5=IFCCARTESIANPOINT((" +
         length(4.125) + ",0.," + length(4) +
         ")); "
         "#10=IFCPOLYLINE((#2,#3,#4,#5));";
}

std::string metricCurveStyle(double sourceUnitsPerMeter, bool invalid = false,
                             bool draughting = false) {
  const auto length = [&](double x) {
    return std::to_string(x * sourceUnitsPerMeter);
  };
  return "#20=IFCCURVESTYLEFONTPATTERN(" + length(invalid ? -1 : 1) + "," +
         length(.5) +
         "); "
         "#21=IFCCURVESTYLEFONTPATTERN(" +
         length(.75) + "," + length(.25) +
         "); "
         "#22=IFCCURVESTYLEFONT('Two-part',(#20,#21)); "
         "#23=IFCCOLOURRGB($,0.75,0.25,0.125); "
         "#24=IFCCURVESTYLE('Metric',#22,IFCPOSITIVELENGTHMEASURE(" +
         length(invalid ? -.2 : .2) + "),#23," + (draughting ? ".F." : ".T.") +
         ");";
}

std::vector<glm::dvec3>
worldCurveEndpoints(const Model &model,
                    const container::geometry::dotbim::Element &element) {
  std::vector<glm::dvec3> result;
  for (const auto &range : model.nativeCurveRanges) {
    if (range.meshId != element.meshId)
      continue;
    for (size_t i = range.firstIndex;
         i < size_t(range.firstIndex) + range.indexCount; ++i) {
      const auto local = model.vertices.at(model.indices.at(i)).position;
      result.emplace_back(element.transform * glm::vec4(local, 1));
    }
  }
  return result;
}

bool pickCurveAtXY(const Model &model,
                   const container::geometry::dotbim::Element &element,
                   glm::dvec2 xy) {
  const auto side = [](glm::dvec2 a, glm::dvec2 b, glm::dvec2 p) {
    const auto ab = b - a, ap = p - a;
    return ab.x * ap.y - ab.y * ap.x;
  };
  for (const auto &range : model.meshRanges) {
    if (range.meshId != element.meshId)
      continue;
    for (size_t i = range.firstIndex;
         i + 2 < size_t(range.firstIndex) + range.indexCount; i += 3) {
      const auto xyAt = [&](size_t offset) {
        const auto local =
            model.vertices.at(model.indices.at(i + offset)).position;
        return glm::dvec2(element.transform * glm::vec4(local, 1));
      };
      const auto a = xyAt(0), b = xyAt(1), c = xyAt(2);
      const double area = side(a, b, c);
      if (std::abs(area) > 1e-12 && side(a, b, xy) * area >= 0 &&
          side(b, c, xy) * area >= 0 && side(c, a, xy) * area >= 0)
        return true;
    }
  }
  return false;
}

const std::array<glm::dvec3, 18> dashedPolylineEndpoints{{
    {0, 0, 0},
    {1, 0, 0},
    {1.5, 0, 0},
    {2.25, 0, 0},
    {2.5, 0, 0},
    {3, 0, 0},
    {3, 0, 0},
    {3, .5, 0},
    {3, 1, 0},
    {3, 1.75, 0},
    {3, 2, 0},
    {3, 3, 0},
    {3, 3.5, 0},
    {3, 4, 0},
    {3, 4, 0},
    {3.25, 4, 0},
    {3.5, 4, 0},
    {4.125, 4, 0},
}};
const std::array<glm::dvec3, 6> solidPolylineEndpoints{{
    {0, 0, 0},
    {3, 0, 0},
    {3, 0, 0},
    {3, 4, 0},
    {3, 4, 0},
    {4.125, 4, 0},
}};

void expectCurveEndpoints(std::span<const glm::dvec3> actual,
                          std::span<const glm::dvec3> expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i)
    EXPECT_LT(glm::length(actual[i] - expected[i]), 2e-6) << i;
}

void setMillimetreUnits(std::string &step) {
  const auto position = step.find(".LENGTHUNIT.,$,.METRE.");
  ASSERT_NE(position, std::string::npos);
  step.replace(position, std::string_view(".LENGTHUNIT.,$,.METRE.").size(),
               ".LENGTHUNIT.,.MILLI.,.METRE.");
}

TEST(IfcCurveStyles, PredefinedFontsUseMetricLengthsAndFontScaling) {
  struct Font {
    std::string_view name;
    std::vector<double> endpointsMm;
  };
  const std::array fonts{
      Font{"dashed", {0, 4, 5.5, 9.5, 11, 11.75}},
      Font{"chain", {0, 7, 8, 9, 10, 11.75}},
      Font{"chain double dash", {0, 7, 8, 9, 10, 11}},
      Font{"dotted", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
      Font{"continuous", {0, 11.75}},
      Font{"unknown", {0, 11.75}},
  };
  for (const auto &font : fonts) {
    SCOPED_TRACE(font.name);
    for (const bool millimetres : {false, true}) {
      for (const double scale : {1., 2.}) {
        SCOPED_TRACE(::testing::Message() << "millimetres=" << millimetres
                                          << " scale=" << scale);
        const double unitPerMm = millimetres ? 1. : .001;
        auto source = fixture(
            "#2=IFCCARTESIANPOINT((0.,0.,0.)); "
            "#3=IFCCARTESIANPOINT((" + std::to_string(11.75 * scale * unitPerMm) +
                ",0.,0.)); #10=IFCPOLYLINE((#2,#3)); "
                "#20=IFCDRAUGHTINGPREDEFINEDCURVEFONT('" + std::string(font.name) +
                "'); #21=IFCCURVESTYLEFONTANDSCALING($,#20,2.); "
                "#24=IFCCURVESTYLE($," + (scale == 1. ? "#20" : "#21") +
                ",$,$,.T.); #25=IFCSTYLEDITEM(#10,(#24),$);");
        if (millimetres)
          setMillimetreUnits(source);
        const auto model = container::geometry::ifc::LoadFromStep(source);
        ASSERT_NO_FATAL_FAILURE(complete(model));
        ASSERT_EQ(model.elements.size(), 1u);
        std::vector<glm::dvec3> expected;
        for (const auto x : font.endpointsMm)
          expected.emplace_back(x * scale * .001, 0, 0);
        expectCurveEndpoints(worldCurveEndpoints(model, model.elements.front()),
                             expected);
      }
    }
  }
}

TEST(IfcCurveStyles, MetricDashPhaseCarriesAcrossCornersAndMatchesPicking) {
  for (const bool millimetres : {false, true}) {
    SCOPED_TRACE(millimetres);
    const double units = millimetres ? 1000. : 1.;
    auto source = fixture(stylePolyline(units) + metricCurveStyle(units) +
                          "#25=IFCSTYLEDITEM(#10,(#24),$);");
    if (millimetres)
      setMillimetreUnits(source);
    const auto model = container::geometry::ifc::LoadFromStep(source);
    ASSERT_NO_FATAL_FAILURE(complete(model));
    ASSERT_EQ(model.elements.size(), 1u);
    const auto &element = model.elements.front();
    EXPECT_EQ(element.geometryKind,
              container::geometry::dotbim::GeometryKind::Curves);
    EXPECT_EQ(element.color, glm::vec4(.75, .25, .125, 1));
    EXPECT_TRUE(element.hasAuthoredCurveWidth);
    expectCurveEndpoints(worldCurveEndpoints(model, element),
                         dashedPolylineEndpoints);
    EXPECT_TRUE(pickCurveAtXY(model, element, {.5, .08}));
    EXPECT_FALSE(pickCurveAtXY(model, element, {.5, .12}));
    EXPECT_FALSE(pickCurveAtXY(model, element, {1.25, 0}));
    EXPECT_TRUE(pickCurveAtXY(model, element, {3, .25}));
    EXPECT_FALSE(pickCurveAtXY(model, element, {3, .75}));
  }
}

TEST(IfcCurveStyles, MappedStyleTransformsDashesAndWidthOnlyForItsOccurrence) {
  auto source = fixture(
      stylePolyline(1000.) + metricCurveStyle(1000.) +
          "#40=IFCSHAPEREPRESENTATION($,'Axis','Curve3D',(#10));"
          "#41=IFCREPRESENTATIONMAP($,#40);"
          "#42=IFCCARTESIANPOINT((10000.,0.,0.));"
          "#43=IFCDIRECTION((-1.,0.,0.)); #44=IFCDIRECTION((0.,1.,0.));"
          "#45=IFCDIRECTION((0.,0.,1.));"
          "#46=IFCCARTESIANTRANSFORMATIONOPERATOR3D(#43,#44,#42,2.,#45);"
          "#47=IFCMAPPEDITEM(#41,#46); #48=IFCMAPPEDITEM(#41,$);"
          "#49=IFCPRESENTATIONSTYLEASSIGNMENT((#24));"
          "#50=IFCPRESENTATIONSTYLEASSIGNMENT((#49));"
          "#51=IFCSTYLEDITEM(#47,(#50),$);",
      "#47,#48", "'Axis','MappedRepresentation'");
  setMillimetreUnits(source);
  const auto model = container::geometry::ifc::LoadFromStep(source);
  ASSERT_NO_FATAL_FAILURE(complete(model));
  ASSERT_EQ(model.elements.size(), 2u);
  const auto styled =
      std::ranges::find_if(model.elements, [](const auto &element) {
        return element.transform[3].x > 9.f;
      });
  ASSERT_NE(styled, model.elements.end());
  const auto &other = model.elements[styled == model.elements.begin() ? 1 : 0];
  EXPECT_EQ(styled->color, glm::vec4(.75, .25, .125, 1));
  EXPECT_TRUE(styled->hasAuthoredCurveWidth);
  EXPECT_FALSE(other.hasAuthoredCurveWidth);
  auto reflected = dashedPolylineEndpoints;
  for (auto &p : reflected)
    p = {10 - 2 * p.x, 2 * p.y, 2 * p.z};
  expectCurveEndpoints(worldCurveEndpoints(model, *styled), reflected);
  expectCurveEndpoints(worldCurveEndpoints(model, other),
                       solidPolylineEndpoints);
  EXPECT_TRUE(pickCurveAtXY(model, *styled, {9., .16}));
  EXPECT_FALSE(pickCurveAtXY(model, *styled, {9., .24}));
  EXPECT_FALSE(pickCurveAtXY(model, *styled, {7.5, 0.}));
  EXPECT_TRUE(pickCurveAtXY(model, other, {1.25, 0.}));
  EXPECT_FALSE(pickCurveAtXY(model, other, {.5, .08}));
}

std::string metricCurveLayer(std::string_view item) {
  return "#26=IFCPRESENTATIONLAYERWITHSTYLE('Curve defaults',$,(" +
         std::string(item) + "),$,.T.,.F.,.F.,(#24));";
}

TEST(IfcCurveStyles, LayerDefaultsInheritAndAllowIndividualOverrides) {
  enum class DirectStyle { None, ByLayer, Width, Continuous };
  for (const bool millimetres : {false, true}) {
    const double units = millimetres ? 1000. : 1.;
    for (const auto style : {DirectStyle::None, DirectStyle::ByLayer,
                             DirectStyle::Width, DirectStyle::Continuous}) {
      SCOPED_TRACE(::testing::Message()
                   << "millimetres=" << millimetres
                   << " directStyle=" << static_cast<int>(style));
      auto definitions = stylePolyline(units) + metricCurveStyle(units) +
                         metricCurveLayer("#10");
      switch (style) {
      case DirectStyle::None:
        break;
      case DirectStyle::ByLayer:
        definitions +=
            "#31=IFCDRAUGHTINGPREDEFINEDCURVEFONT('by layer');"
            "#34=IFCDRAUGHTINGPREDEFINEDCOLOUR('by layer');"
            "#32=IFCCURVESTYLE($,#31,IFCDESCRIPTIVEMEASURE('by layer'),#34,.T.);";
        break;
      case DirectStyle::Width:
        definitions += "#32=IFCCURVESTYLE($,$,IFCPOSITIVELENGTHMEASURE(" +
                       std::to_string(.4 * units) + "),$,.T.);";
        break;
      case DirectStyle::Continuous:
        definitions += "#31=IFCDRAUGHTINGPREDEFINEDCURVEFONT('continuous');"
                       "#34=IFCCOLOURRGB($,0.125,0.75,0.25);"
                       "#32=IFCCURVESTYLE($,#31,$,#34,.T.);";
        break;
      }
      if (style != DirectStyle::None)
        definitions += "#33=IFCSTYLEDITEM(#10,(#32),$);";
      auto source = fixture(definitions);
      if (millimetres)
        setMillimetreUnits(source);
      const auto model = container::geometry::ifc::LoadFromStep(source);
      ASSERT_NO_FATAL_FAILURE(complete(model));
      ASSERT_EQ(model.elements.size(), 1u);
      const auto &element = model.elements.front();
      const bool continuous = style == DirectStyle::Continuous;
      EXPECT_EQ(element.color, continuous ? glm::vec4(.125, .75, .25, 1)
                                          : glm::vec4(.75, .25, .125, 1));
      EXPECT_TRUE(element.hasAuthoredCurveWidth);
      if (continuous)
        expectCurveEndpoints(worldCurveEndpoints(model, element),
                             solidPolylineEndpoints);
      else
        expectCurveEndpoints(worldCurveEndpoints(model, element),
                             dashedPolylineEndpoints);
      const double width = style == DirectStyle::Width ? .4 : .2;
      EXPECT_TRUE(pickCurveAtXY(model, element, {.5, .4 * width}));
      EXPECT_FALSE(pickCurveAtXY(model, element, {.5, .6 * width}));
      EXPECT_EQ(pickCurveAtXY(model, element, {1.25, 0}), continuous);
      EXPECT_TRUE(pickCurveAtXY(model, element, {3, .25}));
      EXPECT_EQ(pickCurveAtXY(model, element, {3, .75}), continuous);
    }
  }
}

TEST(IfcCurveStyles, LayerStyleTracksMappedOccurrencesAndCloserExplicitStyle) {
  for (const bool leafOverride : {false, true}) {
    SCOPED_TRACE(leafOverride);
    auto definitions =
        stylePolyline(1000.) + metricCurveStyle(1000.) +
        metricCurveLayer("#47") +
        "#40=IFCCARTESIANPOINT((0.,0.,0.));"
        "#41=IFCAXIS2PLACEMENT3D(#40,$,$);"
        "#42=IFCSHAPEREPRESENTATION($,'Axis','Curve3D',(#10));"
        "#43=IFCREPRESENTATIONMAP(#41,#42);"
        "#44=IFCCARTESIANPOINT((10000.,0.,0.));"
        "#45=IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$,#44,2.,$);"
        "#46=IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$,#40,1.,$);"
        "#47=IFCMAPPEDITEM(#43,#45); #48=IFCMAPPEDITEM(#43,#46);";
    if (leafOverride)
      definitions +=
          "#31=IFCDRAUGHTINGPREDEFINEDCURVEFONT('continuous');"
          "#34=IFCCOLOURRGB($,0.125,0.75,0.25);"
          "#32=IFCCURVESTYLE($,#31,IFCPOSITIVELENGTHMEASURE(400.),#34,.T.);"
          "#33=IFCSTYLEDITEM(#10,(#32),$);";
    auto source =
        fixture(definitions, "#47,#48", "'Axis','MappedRepresentation'");
    setMillimetreUnits(source);
    const auto model = container::geometry::ifc::LoadFromStep(source);
    ASSERT_NO_FATAL_FAILURE(complete(model));
    ASSERT_EQ(model.elements.size(), 2u);
    const auto mapped =
        std::ranges::find_if(model.elements, [](const auto &element) {
          return element.transform[3].x > 9.f;
        });
    ASSERT_NE(mapped, model.elements.end());
    const auto &other =
        model.elements[mapped == model.elements.begin() ? 1 : 0];
    EXPECT_TRUE(mapped->hasAuthoredCurveWidth);
    EXPECT_EQ(other.hasAuthoredCurveWidth, leafOverride);
    EXPECT_EQ(mapped->color, leafOverride ? glm::vec4(.125, .75, .25, 1)
                                          : glm::vec4(.75, .25, .125, 1));
    const auto mappedEndpoints = [](auto points) {
      for (auto &p : points)
        p = {10 + 2 * p.x, 2 * p.y, 2 * p.z};
      return points;
    };
    if (leafOverride)
      expectCurveEndpoints(worldCurveEndpoints(model, *mapped),
                           mappedEndpoints(solidPolylineEndpoints));
    else
      expectCurveEndpoints(worldCurveEndpoints(model, *mapped),
                           mappedEndpoints(dashedPolylineEndpoints));
    expectCurveEndpoints(worldCurveEndpoints(model, other),
                         solidPolylineEndpoints);
    const double scaledWidth = leafOverride ? .8 : .4;
    EXPECT_TRUE(pickCurveAtXY(model, *mapped, {11., .4 * scaledWidth}));
    EXPECT_FALSE(pickCurveAtXY(model, *mapped, {11., .6 * scaledWidth}));
    EXPECT_EQ(pickCurveAtXY(model, *mapped, {12.5, 0.}), leafOverride);
    EXPECT_TRUE(pickCurveAtXY(model, other, {1.25, 0.}));
    EXPECT_EQ(pickCurveAtXY(model, other, {.5, .16}), leafOverride);
  }
}

TEST(IfcCurveStyles, LayerStyleKeepsIndependentFieldFallbacks) {
  const auto definitions = stylePolyline(1.) + metricCurveStyle(1.) +
                           metricCurveLayer("#10");
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      definitions + "#31=IFCDRAUGHTINGPREDEFINEDCOLOUR('red');"
                    "#32=IFCCURVESTYLE($,$,IFCPOSITIVELENGTHMEASURE(0.4),#31,.F.);"
                    "#33=IFCSTYLEDITEM(#10,(#32),$);"));
  const auto unstyled =
      container::geometry::ifc::LoadFromStep(fixture(stylePolyline(1.)));
  ASSERT_NO_FATAL_FAILURE(complete(model));
  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_EQ(unstyled.elements.size(), 1u);
  const auto &element = model.elements.front();
  EXPECT_FALSE(element.hasAuthoredCurveWidth);
  EXPECT_EQ(element.color, unstyled.elements.front().color);
  expectCurveEndpoints(worldCurveEndpoints(model, element),
                       dashedPolylineEndpoints);

  const auto conflicting = container::geometry::ifc::LoadFromStep(fixture(
      definitions + "#32=IFCCURVESTYLE($,$,IFCPOSITIVELENGTHMEASURE(0.4),$,.T.);"
                    "#33=IFCPRESENTATIONLAYERWITHSTYLE('Other defaults',$,"
                    "(#10),$,.T.,.F.,.F.,(#32));"));
  ASSERT_NO_FATAL_FAILURE(complete(conflicting));
  ASSERT_EQ(conflicting.elements.size(), 1u);
  EXPECT_FALSE(conflicting.elements.front().hasAuthoredCurveWidth);
  EXPECT_EQ(conflicting.elements.front().color, unstyled.elements.front().color);
  expectCurveEndpoints(
      worldCurveEndpoints(conflicting, conflicting.elements.front()),
      solidPolylineEndpoints);
}

TEST(IfcCurveStyles, InvalidAndDraughtingDimensionsFallBackToDrawableCurve) {
  for (const bool draughting : {false, true}) {
    SCOPED_TRACE(draughting);
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        stylePolyline(1.) + metricCurveStyle(1., !draughting, draughting) +
        "#25=IFCSTYLEDITEM(#10,(#24),$);"));
    ASSERT_EQ(model.elements.size(), 1u);
    const auto &element = model.elements.front();
    EXPECT_FALSE(element.hasAuthoredCurveWidth);
    expectCurveEndpoints(worldCurveEndpoints(model, element),
                         solidPolylineEndpoints);
    EXPECT_TRUE(pickCurveAtXY(model, element, {1.25, 0.}));
    EXPECT_FALSE(pickCurveAtXY(model, element, {.5, .08}));
  }
}

TEST(IfcCurveStyles, ScaledDotFontPreservesWidthAndLeavesGaps) {
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(stylePolyline(1.) +
              "#20=IFCCURVESTYLEFONTPATTERN(0.,2.); "
              "#21=IFCCURVESTYLEFONT('Dots',(#20)); "
              "#22=IFCCURVESTYLEFONTANDSCALING('Half interval',#21,0.5); "
              "#24=IFCCURVESTYLE($,#22,IFCPOSITIVELENGTHMEASURE(0.2),$,.T.); "
              "#25=IFCSTYLEDITEM(#10,(#24),$);"));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  ASSERT_EQ(model.elements.size(), 1u);
  const auto &element = model.elements.front();
  EXPECT_TRUE(element.hasAuthoredCurveWidth);
  EXPECT_TRUE(model.nativeCurveRanges.empty());
  EXPECT_TRUE(pickCurveAtXY(model, element, {1, .08}));
  EXPECT_FALSE(pickCurveAtXY(model, element, {1, .12}));
  EXPECT_FALSE(pickCurveAtXY(model, element, {.5, 0}));
  EXPECT_TRUE(pickCurveAtXY(model, element, {3, 0}));
  EXPECT_FALSE(pickCurveAtXY(model, element, {3, .5}));
  EXPECT_TRUE(pickCurveAtXY(model, element, {4.06, 4}));
}

TEST(ParametricCurve, LengthIntegrationAndInversionUseAnalyticSpeed) {
  container::geometry::ParametricCurve curve;
  curve.domain = {{0, 1}};
  curve.point = [](double t) -> std::optional<glm::dvec3> {
    return glm::dvec3(t, t * t, 0);
  };
  curve.derivative = [](double t) -> std::optional<glm::dvec3> {
    return glm::dvec3(1, 2 * t, 0);
  };
  const auto analytic = [](double t) {
    return .5 * t * std::sqrt(1 + 4 * t * t) + .25 * std::asinh(2 * t);
  };
  const auto length = container::geometry::curveLength(curve, 0, 1, 1e-10);
  ASSERT_TRUE(length);
  EXPECT_NEAR(*length, analytic(1), 1e-10);
  const auto parameter = container::geometry::curveParameterAtDistance(
      curve, analytic(.75), 0, 1e-9);
  ASSERT_TRUE(parameter);
  EXPECT_NEAR(*parameter, .75, 1e-9);
  const auto ambiguous = container::geometry::makeBSplineCurve(
      4, {{0, 0, 0}, {1, 0, 0}, {0, 0, 0}, {0, 0, 0}, {1, 0, 0}},
      {0, 0, 0, 0, 0, 1, 1, 1, 1, 1});
  ASSERT_TRUE(ambiguous);
  EXPECT_FALSE(container::geometry::curveParameterAtPoint(*ambiguous,
                                                          {.35, 0, 0}, 1e-7));
}

double volume(const Model &model) {
  double result = 0;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    const glm::dvec3 a = model.vertices[model.indices[i]].position,
                     b = model.vertices[model.indices[i + 1]].position,
                     c = model.vertices[model.indices[i + 2]].position;
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
      const auto p = model.vertices[model.indices[i + k]].position;
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
  for (const auto &[key, edge] : edges) {
    ASSERT_EQ(edge.first, 2u);
    ASSERT_EQ(edge.second, 0);
  }
}

TEST(IfcCurveFamilies,
     SweptSurfacesUseProfileParametersDepthAndRevolutionAxis) {
  const std::string definitions =
      placement2D +
      "#11=IFCCARTESIANPOINT((1.,0.)); #12=IFCCARTESIANPOINT((2.,0.));"
      "#13=IFCPOLYLINE((#11,#12)); "
      "#14=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#13);"
      "#20=IFCCARTESIANPOINT((4.,5.,6.)); #21=IFCAXIS2PLACEMENT3D(#20,$,$);"
      "#22=IFCDIRECTION((0.,0.,1.)); #23=IFCCARTESIANPOINT((0.,0.,0.));"
      "#24=IFCDIRECTION((0.,1.,0.)); #25=IFCAXIS1PLACEMENT(#23,#24);";
  const auto extrusion = container::geometry::ifc::LoadFromStep(
      fixture(definitions +
              "#30=IFCSURFACEOFLINEAREXTRUSION(#14,#21,#22,3.);"
              "#31=IFCCARTESIANPOINT((0.,0.)); #32=IFCCARTESIANPOINT((1.,1.));"
              "#33=IFCPOLYLINE((#31,#32)); #10=IFCPCURVE(#30,#33);"));
  complete(extrusion);
  for (auto p : vertices(extrusion)) {
    EXPECT_NEAR(p.z, 6 + 3 * (p.x - 5), 1e-6);
    EXPECT_NEAR(p.y, 5, 1e-6);
  }
  const auto revolution = container::geometry::ifc::LoadFromStep(fixture(
      definitions + "#30=IFCSURFACEOFREVOLUTION(#14,#21,#25);"
                    "#31=IFCCARTESIANPOINT((0.,0.5)); "
                    "#32=IFCCARTESIANPOINT((6.283185307179586,0.5));"
                    "#33=IFCPOLYLINE((#31,#32)); #10=IFCPCURVE(#30,#33);"));
  complete(revolution);
  const auto p = vertices(revolution);
  EXPECT_NEAR(length(p), 3 * pi, .004);
  for (auto v : p) {
    EXPECT_NEAR(v.y, 5, 1e-6);
    EXPECT_NEAR(glm::length(glm::dvec2(v.x - 4, v.z - 6)), 1.5, 1e-6);
  }
}

// Raw STEP entities let these checks exercise the same analytic reader as
// pcurves and advanced faces, including UVs independent of triangulation.
struct SweptChartEntities {
  using Entity = container::geometry::ifc::detail::Entity;
  using Value = container::geometry::ifc::detail::StepValue;
  std::unordered_map<uint32_t, Entity> entities;

  static Value number(double n) {
    return {.kind = Value::Kind::Number, .number = n};
  }
  static Value ref(uint32_t id) {
    return {.kind = Value::Kind::Ref, .ref = id};
  }
  static Value list(std::initializer_list<Value> values) {
    return {.kind = Value::Kind::List, .list = values};
  }
  void add(uint32_t id, const char *type, std::initializer_list<Value> args) {
    entities.emplace(id, Entity{id, type, list(args)});
  }
  void coordinate(uint32_t id, const char *type,
                  std::initializer_list<double> coordinates) {
    Value args{.kind = Value::Kind::List};
    for (double x : coordinates)
      args.list.push_back(number(x));
    add(id, type, {args});
  }
  void point(uint32_t id, std::initializer_list<double> coordinates) {
    coordinate(id, "IFCCARTESIANPOINT", coordinates);
  }
  void direction(uint32_t id, std::initializer_list<double> coordinates) {
    coordinate(id, "IFCDIRECTION", coordinates);
  }
  static Value curve() { return {.kind = Value::Kind::Enum, .text = "CURVE"}; }
};

using container::geometry::ifc::detail::ParametricSurface;
using container::geometry::ifc::detail::readIfcSurface;
using container::geometry::ifc::detail::readIfcSurfaceCharts;

enum class UnclampedSplineBoundary {
  Smooth,
  Sharp,
  SharpWeights,
  OpenPosition,
  OpenWeights,
};

SweptChartEntities unclampedSpline(bool rational, bool transpose,
                                   UnclampedSplineBoundary boundary) {
  using S = SweptChartEntities;
  S step;
  std::array<glm::dvec2, 6> ring{
      {{2, 0}, {0, 2}, {-2, 0}, {0, -2}, {2, 0}, {0, 2}}};
  if (boundary == UnclampedSplineBoundary::Sharp) {
    ring[4] = {2.5, .5};
    ring[5] = {-.5, 1.5};
  }
  if (boundary == UnclampedSplineBoundary::OpenPosition)
    ring[5].x = .25;

  const std::array<double, 6> weights{1, 2, 3, 4, 2, 4};
  S::Value controls = S::list({}), weightGrid = S::list({});
  for (size_t i = 0; i < (transpose ? 2u : ring.size()); ++i) {
    S::Value row = S::list({}), weightRow = S::list({});
    for (size_t j = 0; j < (transpose ? ring.size() : 2u); ++j) {
      const size_t along = transpose ? j : i, across = transpose ? i : j;
      auto p = ring[along];
      double weight = weights[along];
      if (across != 0)
        weight *= boundary == UnclampedSplineBoundary::OpenWeights && along >= 4
                      ? 4.
                      : 3.;
      if (boundary == UnclampedSplineBoundary::SharpWeights && across != 0 &&
          along >= 4) {
        // Swap the weights and shift the controls to preserve the evaluated
        // position and tangent at this corner, while changing W'/W there.
        weight = weights[along == 4 ? 5 : 4] * 3;
        p += glm::dvec2(-2. / 3, 2. / 3);
      }
      const auto id = uint32_t(1 + 2 * along + across);
      step.point(id, {p.x, p.y, across == 0 ? 0. : 2.});
      row.list.push_back(S::ref(id));
      weightRow.list.push_back(S::number(weight));
    }
    controls.list.push_back(std::move(row));
    weightGrid.list.push_back(std::move(weightRow));
  }

  const auto values = [](std::initializer_list<double> numbers) {
    auto list = S::list({});
    for (double n : numbers)
      list.list.push_back(S::number(n));
    return list;
  };
  const auto periodicKnots = values({-2, -1, 0, 1, 2, 3, 4, 5, 6});
  const auto periodicMultiplicities = values({1, 1, 1, 1, 1, 1, 1, 1, 1});
  const auto axialKnots = values({-1, 3}), axialMultiplicities = values({2, 2});
  const S::Value no{.kind = S::Value::Kind::Enum, .text = "F"};
  const bool open = boundary == UnclampedSplineBoundary::OpenPosition ||
                    boundary == UnclampedSplineBoundary::OpenWeights;
  const S::Value claimedClosed{.kind = S::Value::Kind::Enum,
                               .text = open ? "T" : "F"};
  auto args = S::list({S::number(transpose ? 1 : 2),
                       S::number(transpose ? 2 : 1),
                       controls,
                       {.kind = S::Value::Kind::Enum, .text = "UNSPECIFIED"},
                       transpose ? no : claimedClosed,
                       transpose ? claimedClosed : no,
                       no,
                       transpose ? axialMultiplicities : periodicMultiplicities,
                       transpose ? periodicMultiplicities : axialMultiplicities,
                       transpose ? axialKnots : periodicKnots,
                       transpose ? periodicKnots : axialKnots,
                       {.kind = S::Value::Kind::Enum, .text = "UNSPECIFIED"}});
  if (rational)
    args.list.push_back(std::move(weightGrid));
  step.entities.emplace(20, S::Entity{20,
                                      rational
                                          ? "IFCRATIONALBSPLINESURFACEWITHKNOTS"
                                          : "IFCBSPLINESURFACEWITHKNOTS",
                                      std::move(args)});
  return step;
}

TEST(IfcCurveFamilies,
     UnclampedSplineCertifiesSmoothPolynomialAndRationalSeams) {
  for (bool rational : {false, true}) {
    for (bool transpose : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "rational=" << rational << " transpose=" << transpose);
      const auto step =
          unclampedSpline(rational, transpose, UnclampedSplineBoundary::Smooth);
      std::vector<ParametricSurface> charts;
      std::string error;
      ASSERT_TRUE(readIfcSurfaceCharts(step.entities, 20, 1, {}, charts, error))
          << error;
      ASSERT_EQ(charts.size(), 1u);
      const auto &chart = charts.front();
      ASSERT_TRUE(chart.spline);
      EXPECT_EQ(chart.periods, transpose ? glm::dvec2(0, 4) : glm::dvec2(4, 0));
      EXPECT_EQ(transpose ? chart.vDomain : chart.uDomain, (std::pair{0., 4.}));
      EXPECT_EQ(transpose ? chart.uDomain : chart.vDomain,
                (std::pair{-1., 3.}));
      EXPECT_TRUE(chart.creaseU.empty());
      EXPECT_TRUE(chart.creaseV.empty());

      // Uniform quadratic endpoint basis is (1/2, 1/2), and the first
      // span midpoint uses (1/8, 3/4, 1/8). Neither is a control point.
      const double z = rational ? 1.5 : 1.;
      const glm::dvec3 seam =
          rational ? glm::dvec3(2. / 3, 4. / 3, z) : glm::dvec3(1, 1, z);
      const auto point = [&](double t, double axial) {
        return transpose ? chart.point(axial, t) : chart.point(t, axial);
      };
      for (double t : {0., 4., -4., 8.}) {
        const auto p = point(t, 1.);
        ASSERT_TRUE(p) << t;
        EXPECT_NEAR(glm::length(*p - seam), 0., 1e-12);
      }
      for (double t : {.5, -3.5, 8.5}) {
        const auto p = point(t, 1.);
        ASSERT_TRUE(p) << t;
        EXPECT_NEAR(glm::length(*p - glm::dvec3(rational ? -.25 : 0., 1.5, z)),
                    0., 1e-12);
      }
      EXPECT_FALSE(point(.5, 3.125));

      ASSERT_TRUE(chart.derivatives);
      const double tangent = rational ? 16. / 9 : 2.;
      for (double t : {0., 4.}) {
        const auto d =
            transpose ? chart.derivatives(1, t) : chart.derivatives(t, 1);
        ASSERT_TRUE(d);
        const auto along = transpose ? d->second : d->first;
        const auto across = transpose ? d->first : d->second;
        EXPECT_NEAR(glm::length(along - glm::dvec3(-tangent, tangent, 0)), 0.,
                    1e-12);
        EXPECT_NEAR(
            glm::length(across - glm::dvec3(0, 0, rational ? .375 : .5)), 0.,
            1e-12);
      }
    }
  }
}

TEST(IfcCurveFamilies, UnclampedSplineKeepsOneSidedTangentsAtSharpSeam) {
  const auto step =
      unclampedSpline(false, false, UnclampedSplineBoundary::Sharp);
  std::string error;
  const auto chart = readIfcSurface(step.entities, 20, 1, {}, error);
  ASSERT_TRUE(chart) << error;
  EXPECT_EQ(chart->periods, glm::dvec2(4, 0));
  EXPECT_EQ(chart->creaseU, (std::vector<double>{0.}));
  ASSERT_TRUE(chart->derivatives);
  const auto first = chart->derivatives(0, 1), last = chart->derivatives(4, 1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(last);
  EXPECT_NEAR(glm::length(first->first - glm::dvec3(-2, 2, 0)), 0., 1e-12);
  EXPECT_NEAR(glm::length(last->first - glm::dvec3(-3, 1, 0)), 0., 1e-12);
}

TEST(IfcCurveFamilies, UnclampedSplineDetectsDerivativeMismatchBetweenCorners) {
  const auto step =
      unclampedSpline(true, false, UnclampedSplineBoundary::SharpWeights);
  std::string error;
  const auto chart = readIfcSurface(step.entities, 20, 1, {}, error);
  ASSERT_TRUE(chart) << error;
  EXPECT_EQ(chart->periods, glm::dvec2(4, 0));
  EXPECT_EQ(chart->creaseU, (std::vector<double>{0.}));
  ASSERT_TRUE(chart->derivatives);
  for (double axial : {-1., 1., 3.}) {
    const auto first = chart->derivatives(0, axial);
    const auto last = chart->derivatives(4, axial);
    ASSERT_TRUE(first);
    ASSERT_TRUE(last);
    const glm::dvec3 expected(0, 0, axial == 1. ? -.5 : 0.);
    EXPECT_NEAR(glm::length(last->first - first->first - expected), 0., 1e-12);
  }
}

TEST(IfcCurveFamilies, UnclampedSplineDoesNotWrapAFalseClosureClaim) {
  const auto step =
      unclampedSpline(true, false, UnclampedSplineBoundary::OpenPosition);
  std::string error;
  const auto chart = readIfcSurface(step.entities, 20, 1, {}, error);
  ASSERT_TRUE(chart) << error;
  EXPECT_EQ(chart->periods, glm::dvec2(0));
  const auto first = chart->point(0, 1), last = chart->point(4, 1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(last);
  EXPECT_NEAR(last->x - first->x, 1. / 6, 1e-12);
  EXPECT_FALSE(chart->point(4.125, 1));
}

TEST(IfcCurveFamilies, UnclampedSplineRejectsCoincidentCornersWithOpenTrace) {
  const auto step =
      unclampedSpline(true, false, UnclampedSplineBoundary::OpenWeights);
  std::string error;
  const auto chart = readIfcSurface(step.entities, 20, 1, {}, error);
  ASSERT_TRUE(chart) << error;
  EXPECT_EQ(chart->periods, glm::dvec2(0));
  for (double axial : {-1., 3.}) {
    const auto first = chart->point(0, axial), last = chart->point(4, axial);
    ASSERT_TRUE(first);
    ASSERT_TRUE(last);
    EXPECT_NEAR(glm::length(*first - *last), 0., 1e-12);
  }
  const auto first = chart->point(0, 1), last = chart->point(4, 1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(last);
  EXPECT_NEAR(last->z - first->z, .1, 1e-12);
  EXPECT_FALSE(chart->point(4.125, 1));
}

void expectChartPoint(const ParametricSurface &chart, glm::dvec2 parameter,
                      glm::dvec3 expected) {
  const auto point = chart.point(parameter.x, parameter.y);
  ASSERT_TRUE(point);
  EXPECT_LT(glm::length(*point - expected), 1e-7);
  ASSERT_TRUE(chart.inverse);
  const auto inverse = chart.inverse(expected);
  ASSERT_TRUE(inverse);
  glm::dvec2 delta = *inverse - parameter;
  for (int axis = 0; axis < 2; ++axis)
    if (chart.periods[axis] > 0)
      delta[axis] = std::remainder(delta[axis], chart.periods[axis]);
  EXPECT_LT(glm::length(delta), 1e-6);
}

TEST(IfcCurveFamilies, HollowCircleSweptChartsKeepIndependentAngleDomains) {
  SweptChartEntities step;
  using S = SweptChartEntities;
  step.point(1, {2, 3});
  step.add(2, "IFCAXIS2PLACEMENT2D", {S::ref(1), {}});
  step.add(3, "IFCCIRCLEHOLLOWPROFILEDEF",
           {S::curve(), {}, S::ref(2), S::number(5), S::number(1.25)});
  step.point(4, {11, 13, 17});
  step.direction(5, {1, 0, 0});
  step.direction(6, {0, 1, 0});
  step.add(7, "IFCAXIS2PLACEMENT3D", {S::ref(4), S::ref(5), S::ref(6)});
  step.direction(8, {0, 3, 4});
  step.add(10, "IFCSURFACEOFLINEAREXTRUSION",
           {S::ref(3), S::ref(7), S::ref(8), S::number(10)});

  std::vector<ParametricSurface> charts;
  std::string error;
  ASSERT_TRUE(
      readIfcSurfaceCharts(step.entities, 10, .001, pi / 180, charts, error))
      << error;
  ASSERT_EQ(charts.size(), 2u);
  for (size_t i = 0; i < charts.size(); ++i) {
    const auto &chart = charts[i];
    EXPECT_EQ(chart.profileSourceId, 3u);
    EXPECT_EQ(chart.innerProfile, i == 1);
    ASSERT_TRUE(chart.uDomain);
    EXPECT_EQ(chart.uDomain->first, 0.);
    EXPECT_NEAR(chart.uDomain->second, 360., 1e-10);
    EXPECT_NEAR(chart.periods.x, 360., 1e-10);
    EXPECT_NEAR(chart.angularScale.x, pi / 180, 1e-12);
    EXPECT_FALSE(chart.vDomain);
    const double radius = i == 0 ? 5. : 3.75;
    for (const double u : {90., 225., 450.}) {
      const double radians = u * pi / 180;
      expectChartPoint(chart, {u, 1.25},
                       {21, 15 + radius * std::cos(radians),
                        27.5 + radius * std::sin(radians)});
    }
  }
  EXPECT_FALSE(readIfcSurface(step.entities, 10, .001, pi / 180, error));
  EXPECT_FALSE(error.empty());
}

TEST(IfcCurveFamilies, HollowRectangleSweptChartsKeepIndependentExactFillets) {
  SweptChartEntities step;
  using S = SweptChartEntities;
  step.add(3, "IFCRECTANGLEHOLLOWPROFILEDEF",
           {S::curve(),
            {},
            {},
            S::number(6),
            S::number(4),
            S::number(.5),
            S::number(.3),
            S::number(.6)});
  step.direction(8, {0, 0, 1});
  step.add(10, "IFCSURFACEOFLINEAREXTRUSION",
           {S::ref(3), {}, S::ref(8), S::number(4)});
  std::vector<ParametricSurface> charts;
  std::string error;
  ASSERT_TRUE(readIfcSurfaceCharts(step.entities, 10, 1., 1., charts, error))
      << error;
  ASSERT_EQ(charts.size(), 2u);
  for (size_t i = 0; i < charts.size(); ++i) {
    SCOPED_TRACE(i);
    const auto &chart = charts[i];
    const double x = 3 - .5 * i, y = 2 - .5 * i, r = i == 0 ? .6 : .3;
    EXPECT_EQ(chart.profileSourceId, 3u);
    EXPECT_EQ(chart.innerProfile, i == 1);
    ASSERT_TRUE(chart.uDomain);
    EXPECT_EQ(chart.uDomain->first, 0.);
    EXPECT_EQ(chart.uDomain->second, 8.);
    expectChartPoint(
        chart, {.5, .25},
        {x - r + r / std::sqrt(2.), y - r + r / std::sqrt(2.), 1.});
    expectChartPoint(chart, {1.5, .75}, {0, y, 3});
  }
}

TEST(IfcCurveFamilies,
     CompositeSweptChartsKeepSourceIdsThroughNestedTransforms) {
  SweptChartEntities step;
  using S = SweptChartEntities;
  step.point(1, {-3, 1});
  step.add(2, "IFCAXIS2PLACEMENT2D", {S::ref(1), {}});
  step.add(13, "IFCRECTANGLEPROFILEDEF",
           {S::curve(), {}, S::ref(2), S::number(2), S::number(1)});
  step.point(4, {4, 2});
  step.add(5, "IFCAXIS2PLACEMENT2D", {S::ref(4), {}});
  step.add(20, "IFCCIRCLEPROFILEDEF",
           {S::curve(), {}, S::ref(5), S::number(.6)});
  step.add(21, "IFCCOMPOSITEPROFILEDEF",
           {S::curve(), {}, S::list({S::ref(20), S::ref(13)}), {}});
  step.point(22, {8, -6});
  step.add(23, "IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM",
           {{}, {}, S::ref(22), S::number(2), S::number(.5)});
  step.add(24, "IFCDERIVEDPROFILEDEF",
           {S::curve(), {}, S::ref(21), S::ref(23), {}});
  step.add(25, "IFCMIRROREDPROFILEDEF", {S::curve(), {}, S::ref(24), {}, {}});
  step.direction(8, {0, 0, 1});
  step.add(10, "IFCSURFACEOFLINEAREXTRUSION",
           {S::ref(25), {}, S::ref(8), S::number(3000)});

  std::vector<ParametricSurface> charts;
  std::string error;
  ASSERT_TRUE(readIfcSurfaceCharts(step.entities, 10, .001, 1., charts, error))
      << error;
  ASSERT_EQ(charts.size(), 2u);
  EXPECT_EQ(charts[0].profileSourceId, 13u);
  EXPECT_EQ(charts[1].profileSourceId, 20u);
  EXPECT_FALSE(charts[0].innerProfile);
  EXPECT_FALSE(charts[1].innerProfile);
  expectChartPoint(charts[0], {.5, .5}, {-2, -5.75, 1500});
  expectChartPoint(charts[1], {.7, .25},
                   {-16 - 1.2 * std::cos(.7), -5 + .3 * std::sin(.7), 750});

  // The second profile now crosses the first one's envelope.
  // Failure must retire both charts even though the rectangle is valid alone.
  step.entities.erase(4);
  step.point(4, {-2.2, 1});
  charts.clear();
  EXPECT_FALSE(
      readIfcSurfaceCharts(step.entities, 10, .001, 1., charts, error));
  EXPECT_TRUE(charts.empty());
  EXPECT_FALSE(error.empty());
}

TEST(IfcCurveFamilies, CenterlineSweptChartKeepsNativeKnotDomainAndDepth) {
  SweptChartEntities step;
  using S = SweptChartEntities;
  step.point(1, {2, 0});
  step.point(2, {4, 2});
  step.add(3, "IFCBSPLINECURVEWITHKNOTS",
           {S::number(1),
            S::list({S::ref(1), S::ref(2)}),
            {.kind = S::Value::Kind::Enum, .text = "UNSPECIFIED"},
            {.kind = S::Value::Kind::Enum, .text = "F"},
            {.kind = S::Value::Kind::Enum, .text = "F"},
            S::list({S::number(2), S::number(2)}),
            S::list({S::number(-2), S::number(6)}),
            {.kind = S::Value::Kind::Enum, .text = "UNSPECIFIED"}});
  step.add(4, "IFCCENTERLINEPROFILEDEF",
           {S::curve(), {}, S::ref(3), S::number(.5)});
  step.direction(8, {0, 0, 1});
  step.add(10, "IFCSURFACEOFLINEAREXTRUSION",
           {S::ref(4), {}, S::ref(8), S::number(4)});
  step.point(11, {1, 0, 0});
  step.direction(12, {0, 1, 0});
  step.add(13, "IFCAXIS1PLACEMENT", {S::ref(11), S::ref(12)});
  step.add(14, "IFCSURFACEOFREVOLUTION", {S::ref(4), {}, S::ref(13)});

  std::vector<ParametricSurface> charts;
  std::string error;
  ASSERT_TRUE(readIfcSurfaceCharts(step.entities, 10, 1., 1., charts, error))
      << error;
  ASSERT_EQ(charts.size(), 1u);
  const auto &chart = charts.front();
  EXPECT_EQ(chart.profileSourceId, 4u);
  EXPECT_FALSE(chart.innerProfile);
  ASSERT_TRUE(chart.uDomain);
  EXPECT_EQ(chart.uDomain->first, -2.);
  EXPECT_EQ(chart.uDomain->second, 6.);
  EXPECT_FALSE(chart.point(-2.001, .5));
  EXPECT_FALSE(chart.point(6.001, .5));
  expectChartPoint(chart, {2., -.25}, {3, 1, -1});

  charts.clear();
  ASSERT_TRUE(
      readIfcSurfaceCharts(step.entities, 14, 1., pi / 180, charts, error))
      << error;
  ASSERT_EQ(charts.size(), 1u);
  const auto &revolution = charts.front();
  EXPECT_EQ(revolution.profileSourceId, 4u);
  EXPECT_NEAR(revolution.periods.x, 360., 1e-10);
  ASSERT_TRUE(revolution.vDomain);
  EXPECT_EQ(revolution.vDomain->first, -2.);
  EXPECT_EQ(revolution.vDomain->second, 6.);
  EXPECT_FALSE(revolution.point(60., -2.001));
  expectChartPoint(revolution, {60., 2.}, {2., 1., -std::sqrt(3.)});
  expectChartPoint(revolution, {-420., 2.}, {2., 1., std::sqrt(3.)});
}

const std::string hollowSweptPcurve =
    "#3=IFCCIRCLEHOLLOWPROFILEDEF(.CURVE.,$,$,3.,1.);"
    "#4=IFCDIRECTION((0.,0.,1.));"
    "#5=IFCSURFACEOFLINEAREXTRUSION(#3,$,#4,4.);"
    "#6=IFCCARTESIANPOINT((0.,0.2)); #7=IFCCARTESIANPOINT((0.,0.8));"
    "#8=IFCPOLYLINE((#6,#7)); #9=IFCPCURVE(#5,#8);";

TEST(IfcCurveFamilies,
     SurfaceCurveUsesExplicitWitnessToSelectHollowSweepChart) {
  for (const std::string radius : {"2.", "3."}) {
    SCOPED_TRACE(radius);
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(hollowSweptPcurve + "#11=IFCCARTESIANPOINT((" + radius +
                ",0.,0.8)); #12=IFCCARTESIANPOINT((" + radius +
                ",0.,3.2)); #13=IFCPOLYLINE((#11,#12));"
                "#10=IFCSURFACECURVE(#13,(#9),.PCURVE_S1.);"));
    complete(model);
    const auto points = vertices(model);
    ASSERT_FALSE(points.empty());
    EXPECT_NEAR(points.front().z, .8, 1e-6);
    EXPECT_NEAR(points.back().z, 3.2, 1e-6);
    EXPECT_NEAR(length(points), 2.4, 1e-6);
    for (const auto p : points) {
      EXPECT_NEAR(p.x, std::stod(radius), 1e-6);
      EXPECT_NEAR(p.y, 0., 1e-6);
    }
  }
}

TEST(IfcCurveFamilies, AmbiguousAndMismatchedSweptPcurvesAreAtomic) {
  for (const std::string_view item : {"#9", "#10", "#18"}) {
    SCOPED_TRACE(item);
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        hollowSweptPcurve +
            "#11=IFCCARTESIANPOINT((2.5,0.,0.8));"
            "#12=IFCCARTESIANPOINT((2.5,0.,3.2)); #13=IFCPOLYLINE((#11,#12));"
            "#10=IFCSURFACECURVE(#13,(#9),.PCURVE_S1.);"
            "#14=IFCCARTESIANPOINT((2.,0.,0.8));"
            "#15=IFCCARTESIANPOINT((2.5,0.,2.));"
            "#16=IFCCARTESIANPOINT((2.,0.,3.2));"
            "#17=IFCPOLYLINE((#14,#15,#16));"
            "#18=IFCSURFACECURVE(#17,(#9),.PCURVE_S1.);",
        item));
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    EXPECT_TRUE(model.nativeCurveRanges.empty());
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId,
              static_cast<unsigned>(std::stoi(std::string(item.substr(1)))));
  }
}

TEST(IfcCurveFamilies, SweepPcurveSamplesNativeNonintegerCreaseExactly) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.)); #3=IFCCARTESIANPOINT((3.,0.));"
      "#4=IFCCARTESIANPOINT((3.,2.));"
      "#5=IFCBSPLINECURVEWITHKNOTS(1,(#2,#3,#4),.UNSPECIFIED.,.F.,.F.,"
      "(2,1,2),(-2.,1.3,8.),.UNSPECIFIED.);"
      "#6=IFCCENTERLINEPROFILEDEF(.CURVE.,$,#5,1.);"
      "#7=IFCDIRECTION((0.,0.,1.)); #8=IFCSURFACEOFLINEAREXTRUSION(#6,$,#7,4.);"
      "#11=IFCCARTESIANPOINT((-1.,0.25)); #12=IFCCARTESIANPOINT((7.,0.25));"
      "#9=IFCPOLYLINE((#11,#12)); #10=IFCPCURVE(#8,#9);"));
  complete(model);
  const auto points = vertices(model);
  ASSERT_FALSE(points.empty());
  EXPECT_EQ(std::ranges::count(points, glm::dvec3(3, 0, 1)), 2);
  const double firstLeg = 3. * (1.3 + 1.) / 3.3;
  const double secondLeg = 2. * (7. - 1.3) / 6.7;
  EXPECT_NEAR(length(points), firstLeg + secondLeg, 1e-6);
  for (size_t i = 0; i < points.size(); i += 2) {
    const auto &a = points[i], &b = points[i + 1];
    EXPECT_TRUE((a.y == 0 && b.y == 0) || (a.x == 3 && b.x == 3))
        << a.x << ',' << a.y << " to " << b.x << ',' << b.y;
  }
}

void completeSweptMesh(const Model &model) {
  for (const auto &diagnostic : model.importReport.diagnostics)
    ADD_FAILURE() << diagnostic.entityId << ": " << diagnostic.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  ASSERT_FALSE(model.meshRanges.empty());
  ASSERT_FALSE(model.indices.empty());
  ASSERT_EQ(model.indices.size() % 3, 0u);
  EXPECT_TRUE(model.nativeCurveRanges.empty());
}

TEST(IfcCurveFamilies, HollowSweptExtrusionDrawsBothWallsWithOppositeWinding) {
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(hollowSweptPcurve, "#5", "'Surface','Surface3D'"));
  completeSweptMesh(model);
  std::array<double, 2> area{};
  std::array<bool, 2> lower{}, upper{};
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    std::array<glm::dvec3, 3> p;
    const size_t wall =
        glm::length(
            glm::dvec2(model.vertices.at(model.indices.at(i)).position)) < 2.5
            ? 0
            : 1;
    const double radius = wall == 0 ? 2. : 3.;
    const double direction = wall == 0 ? -1. : 1.;
    for (size_t j = 0; j < p.size(); ++j) {
      const auto &vertex = model.vertices.at(model.indices.at(i + j));
      p[j] = glm::dvec3(vertex.position);
      EXPECT_NEAR(glm::length(glm::dvec2(p[j])), radius, 1e-6);
      EXPECT_GE(p[j].z, 0.);
      EXPECT_LE(p[j].z, 4.);
      lower[wall] |= p[j].z == 0.;
      upper[wall] |= p[j].z == 4.;
      const auto expected = glm::normalize(glm::dvec3(p[j].x, p[j].y, 0));
      EXPECT_GT(direction * glm::dot(glm::dvec3(vertex.normal), expected),
                .999);
    }
    const auto normal = glm::cross(p[1] - p[0], p[2] - p[0]);
    const auto centroid = (p[0] + p[1] + p[2]) / 3.;
    EXPECT_GT(direction *
                  glm::dot(normal, glm::dvec3(centroid.x, centroid.y, 0)),
              0.);
    area[wall] += .5 * glm::length(normal);
  }
  EXPECT_TRUE(lower[0] && lower[1] && upper[0] && upper[1]);
  EXPECT_NEAR(area[0], 16 * pi, .02);
  EXPECT_NEAR(area[1], 24 * pi, .02);
}

const std::string sweptDegreeUnits =
    "#30=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.);"
    "#31=IFCMEASUREWITHUNIT(IFCPLANEANGLEMEASURE(0.017453292519943295),#30);"
    "#32=IFCCONVERSIONBASEDUNIT($,.PLANEANGLEUNIT.,'degree',#31);"
    "#33=IFCUNITASSIGNMENT((#1,#32));"
    "#34=IFCPROJECT('project',$,$,$,$,$,$,(),#33);";

TEST(IfcCurveFamilies,
     TrimmedSweptRevolutionWrapsDegreeSeamAndReversesWinding) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      sweptDegreeUnits +
          "#2=IFCCARTESIANPOINT((2.,1.)); #3=IFCCARTESIANPOINT((2.,4.));"
          "#4=IFCPOLYLINE((#2,#3)); "
          "#5=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#4);"
          "#6=IFCCARTESIANPOINT((0.,0.,0.)); #7=IFCDIRECTION((0.,1.,0.));"
          "#8=IFCAXIS1PLACEMENT(#6,#7); #9=IFCSURFACEOFREVOLUTION(#5,$,#8);"
          "#10=IFCRECTANGULARTRIMMEDSURFACE(#9,350.,0.75,10.,0.25,.T.,.F.);",
      "#10", "'Surface','Surface3D'"));
  completeSweptMesh(model);
  const double seam = pi / 18;
  double low = seam, high = -seam, area = 0;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    std::array<glm::dvec3, 3> p;
    for (size_t j = 0; j < p.size(); ++j) {
      const auto &vertex = model.vertices.at(model.indices.at(i + j));
      p[j] = glm::dvec3(vertex.position);
      EXPECT_GE(p[j].y, 1.75);
      EXPECT_LE(p[j].y, 3.25);
      EXPECT_NEAR(std::hypot(p[j].x, p[j].z), 2., 1e-6);
      const double angle = std::atan2(-p[j].z, p[j].x);
      low = std::min(low, angle);
      high = std::max(high, angle);
      EXPECT_LE(std::abs(angle), seam + 1e-6);
      EXPECT_LT(
          glm::dot(glm::dvec3(vertex.normal), glm::dvec3(p[j].x, 0, p[j].z)),
          -1.999);
    }
    const auto normal = glm::cross(p[1] - p[0], p[2] - p[0]);
    const auto centroid = (p[0] + p[1] + p[2]) / 3.;
    EXPECT_LT(glm::dot(normal, glm::dvec3(centroid.x, 0, centroid.z)), 0.);
    area += .5 * glm::length(normal);
  }
  EXPECT_NEAR(low, -seam, 1e-6);
  EXPECT_NEAR(high, seam, 1e-6);
  EXPECT_NEAR(area, pi / 3, .002);
}

TEST(IfcCurveFamilies,
     StandaloneRevolutionWindingOnBothSidesAndSingularRejection) {
  for (const auto &[start, end, axis] :
       std::array<std::array<std::string_view, 3>, 4>{
           {{"2.", "2.", "0.,1.,0."},
            {"-2.", "-2.", "0.,1.,0."},
            {"-2.", "2.", "0.,1.,0."},
            {"2.", "2.", "0.,0.,1."}}}) {
    SCOPED_TRACE(::testing::Message()
                 << start << " → " << end << " about " << axis);
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        "#2=IFCCARTESIANPOINT((" + std::string(start) +
            ",1.)); #3=IFCCARTESIANPOINT((" + std::string(end) +
            ",4.));"
            "#4=IFCPOLYLINE((#2,#3)); "
            "#5=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#4);"
            "#6=IFCCARTESIANPOINT((0.,0.,0.)); #7=IFCDIRECTION((" +
            std::string(axis) +
            "));"
            "#8=IFCAXIS1PLACEMENT(#6,#7); #10=IFCSURFACEOFREVOLUTION(#5,$,#8);",
        "#10", "'Surface','Surface3D'"));
    if (start != end || axis != "0.,1.,0.") {
      EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
      EXPECT_TRUE(model.vertices.empty());
      EXPECT_TRUE(model.indices.empty());
      ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
      EXPECT_EQ(model.importReport.diagnostics.front().entityId, 10u);
      continue;
    }
    completeSweptMesh(model);
    double area = 0;
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      std::array<glm::dvec3, 3> p;
      for (size_t j = 0; j < p.size(); ++j) {
        const auto &vertex = model.vertices.at(model.indices.at(i + j));
        p[j] = glm::dvec3(vertex.position);
        const glm::dvec3 radial(p[j].x, 0, p[j].z);
        EXPECT_NEAR(glm::length(radial), 2., 1e-6);
        EXPECT_GT(glm::dot(glm::dvec3(vertex.normal), radial), 1.999);
      }
      const auto n = glm::cross(p[1] - p[0], p[2] - p[0]);
      const auto centroid = (p[0] + p[1] + p[2]) / 3.;
      EXPECT_GT(glm::dot(n, glm::dvec3(centroid.x, 0, centroid.z)), 0.);
      area += .5 * glm::length(n);
    }
    EXPECT_NEAR(area, 12 * pi, .02);
  }
}

// The quadratic Bezier meridian (2t, t²), 0 <= t <= 1, gives the
// paraboloid height r²/4. The default IFC knot interval is [2, 5].
std::string smoothRevolutionDefinition(
    const std::array<glm::dvec2, 3> &controls, bool placed = false,
    std::string_view knots = "2.,5.") {
  std::ostringstream step;
  for (size_t i = 0; i < controls.size(); ++i)
    step << '#' << i + 2 << "=IFCCARTESIANPOINT((" << controls[i].x << ','
         << controls[i].y << "));";
  step << "#5=IFCBSPLINECURVEWITHKNOTS(2,(#2,#3,#4),.UNSPECIFIED.,.F.,.F.,"
          "(3,3),(" << knots << "),.UNSPECIFIED.);"
          "#6=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#5);"
          "#7=IFCCARTESIANPOINT((0.,0.,0.));"
          "#8=IFCDIRECTION((0.,1.,0.)); #10=IFCAXIS1PLACEMENT(#7,#8);";
  if (placed) {
    step << "#13=IFCMIRROREDPROFILEDEF(.CURVE.,$,#6,*,$);"
            "#14=IFCCARTESIANPOINT((3000.,5000.,-2000.));"
            "#15=IFCDIRECTION((1.,0.,0.)); #16=IFCDIRECTION((0.,1.,0.));"
            "#17=IFCAXIS2PLACEMENT3D(#14,#15,#16);"
            "#9=IFCSURFACEOFREVOLUTION(#13,#17,#10);";
  } else {
    step << "#9=IFCSURFACEOFREVOLUTION(#6,$,#10);";
  }
  return step.str();
}

struct StandaloneParaboloidFrame {
  glm::dvec3 apex{0, 0, 0}, radial{1, 0, 0}, axis{0, 1, 0}, side{0, 0, 1};
  double unitsPerMeter{1};

  glm::dvec3 local(glm::dvec3 point) const {
    const auto p = (point - apex) / unitsPerMeter;
    return {glm::dot(p, radial), glm::dot(p, axis), glm::dot(p, side)};
  }

  glm::dvec3 normal(glm::dvec3 p) const {
    return glm::normalize((p.x / 2) * radial - axis + (p.z / 2) * side);
  }
};

void expectStandaloneParaboloid(const Model &model,
                                 const StandaloneParaboloidFrame &frame,
                                 double sense, double radius,
                                 std::optional<double> halfAngle = {},
                                 double innerRadius = 0) {
  ASSERT_NO_FATAL_FAILURE(completeSweptMesh(model));
  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  using Position = std::array<float, 3>;
  std::set<Position> positions;
  std::map<std::pair<Position, Position>, std::pair<unsigned, int>> edges;
  std::optional<uint32_t> apexIndex;
  glm::dvec3 minimum(std::numeric_limits<double>::max());
  glm::dvec3 maximum(std::numeric_limits<double>::lowest());
  double area = 0;
  unsigned apexTriangles = 0, boundaryEdges = 0;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    std::array<glm::dvec3, 3> local, points;
    std::array<Position, 3> keys;
    unsigned apexCorners = 0;
    for (size_t j = 0; j < local.size(); ++j) {
      const uint32_t index = model.indices[i + j];
      ASSERT_LT(index, model.vertices.size());
      const auto &vertex = model.vertices[index];
      points[j] = vertex.position;
      keys[j] = {vertex.position.x, vertex.position.y, vertex.position.z};
      positions.insert(keys[j]);
      const auto p = local[j] = frame.local(points[j]);
      minimum = glm::min(minimum, p);
      maximum = glm::max(maximum, p);
      const double r = std::hypot(p.x, p.z);
      EXPECT_NEAR(p.y, r * r / 4, 2e-6);
      EXPECT_GE(r, innerRadius - 2e-6);
      EXPECT_LE(r, radius + 2e-6);
      EXPECT_GT(sense * glm::dot(glm::dvec3(vertex.normal), frame.normal(p)),
                .99999);
      if (r < 1e-7) {
        ++apexCorners;
        if (!apexIndex)
          apexIndex = index;
        EXPECT_EQ(index, *apexIndex);
        EXPECT_EQ(points[j], frame.apex);
        EXPECT_EQ(glm::dvec3(vertex.normal), -sense * frame.axis);
      } else if (halfAngle) {
        EXPECT_LE(std::abs(std::atan2(-p.z, p.x)), *halfAngle + 2e-6);
      }
    }
    ASSERT_LE(apexCorners, 1u);
    apexTriangles += apexCorners;
    const auto face = glm::cross(points[1] - points[0], points[2] - points[0]);
    const auto centroid = (local[0] + local[1] + local[2]) / 3.;
    EXPECT_GT(sense * glm::dot(face, frame.normal(centroid)), 0.);
    area += glm::length(face) / (2 * frame.unitsPerMeter * frame.unitsPerMeter);
    for (size_t j = 0; j < keys.size(); ++j) {
      const auto &a = keys[j], &b = keys[(j + 1) % keys.size()];
      ASSERT_NE(a, b);
      auto &edge = edges[std::minmax(a, b)];
      ++edge.first;
      edge.second += a < b ? 1 : -1;
    }
  }
  if (innerRadius == 0) {
    ASSERT_TRUE(apexIndex);
    EXPECT_GT(apexTriangles, 2u);
    EXPECT_EQ(std::ranges::count_if(model.vertices, [&](const auto &vertex) {
      return glm::dvec3(vertex.position) == frame.apex;
    }), 1);
  } else {
    EXPECT_FALSE(apexIndex);
  }
  // Weld by exact position to check disk topology and paired edges across
  // the full-turn seam, independently of ordinary vertex indexing.
  const size_t euler = halfAngle || innerRadius == 0 ? 1u : 0u;
  EXPECT_EQ(positions.size() + model.indices.size() / 3, edges.size() + euler);
  const auto boundary = [&](Position key) {
    const auto p = frame.local({key[0], key[1], key[2]});
    const double r = std::hypot(p.x, p.z);
    unsigned sides = 0;
    if (std::abs(r - radius) < 2e-6)
      sides |= 1;
    if (innerRadius > 0 && std::abs(r - innerRadius) < 2e-6)
      sides |= 2;
    if (halfAngle) {
      const double angle = std::atan2(-p.z, p.x);
      if (r < 1e-7 || std::abs(angle + *halfAngle) < 2e-6)
        sides |= 4;
      if (r < 1e-7 || std::abs(angle - *halfAngle) < 2e-6)
        sides |= 8;
    }
    return sides;
  };
  for (const auto &[ends, use] : edges) {
    EXPECT_LE(use.first, 2u);
    if (use.first == 2) {
      EXPECT_EQ(use.second, 0);
    } else {
      ++boundaryEdges;
      EXPECT_NE(boundary(ends.first) & boundary(ends.second), 0u);
    }
  }
  EXPECT_GT(boundaryEdges, 2u);
  const auto expectedMinimum = halfAngle
      ? glm::dvec3(innerRadius * std::cos(*halfAngle),
                   innerRadius * innerRadius / 4, -radius * std::sin(*halfAngle))
      : glm::dvec3(-radius, innerRadius * innerRadius / 4, -radius);
  const glm::dvec3 expectedMaximum(radius, radius * radius / 4,
      halfAngle ? radius * std::sin(*halfAngle) : radius);
  EXPECT_LT(glm::length(minimum - expectedMinimum), 3e-6);
  EXPECT_LT(glm::length(maximum - expectedMaximum), 3e-6);
  const double sweepAngle = 2 * halfAngle.value_or(pi);
  const auto primitive = [](double r) { return std::pow(1 + r * r / 4, 1.5); };
  const double expectedArea = 4 * sweepAngle / 3 *
                              (primitive(radius) - primitive(innerRadius));
  EXPECT_NEAR(area, expectedArea, halfAngle ? .002 : .02);
}

TEST(IfcCurveFamilies, StandaloneSmoothRevolutionWeldsApexAndFullTurnSeam) {
  constexpr std::array<glm::dvec2, 3> controls{{{0, 0}, {1, 0}, {2, 1}}};
  const auto direct = container::geometry::ifc::LoadFromStep(
      fixture(smoothRevolutionDefinition(controls), "#9", "'Surface','Surface3D'"));
  ASSERT_NO_FATAL_FAILURE(expectStandaloneParaboloid(direct, {}, 1, 2));

  auto millimetres = fixture(smoothRevolutionDefinition(
      {{{2000, 1000}, {1000, 0}, {0, 0}}}, true), "#9", "'Surface','Surface3D'");
  setMillimetreUnits(millimetres);
  const auto placed = container::geometry::ifc::LoadFromStep(millimetres);
  ASSERT_NO_FATAL_FAILURE(expectStandaloneParaboloid(
      placed, {{3000, 5000, -2000}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, 1000}, -1, 2));
  EXPECT_TRUE(placed.unitMetadata.hasMetersPerUnit);
  EXPECT_FLOAT_EQ(placed.unitMetadata.metersPerUnit, .001f);
}

TEST(IfcCurveFamilies, TrimmedSmoothRevolutionKeepsApexAndReversesItsSense) {
  constexpr std::array<glm::dvec2, 3> controls{{{0, 0}, {1, 0}, {2, 1}}};
  const auto definitions = sweptDegreeUnits + smoothRevolutionDefinition(controls);
  for (const bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        definitions + "#11=IFCRECTANGULARTRIMMEDSURFACE(#9,350.," +
            (reverse ? "4.25,10.,2.,.T.,.F.);" : "2.,10.,4.25,.T.,.T.);"),
        "#11", "'Surface','Surface3D'"));
    ASSERT_NO_FATAL_FAILURE(expectStandaloneParaboloid(
        model, {}, reverse ? -1 : 1, 1.5, pi / 18));
  }
  const auto withoutApex = container::geometry::ifc::LoadFromStep(fixture(
      definitions + "#11=IFCRECTANGULARTRIMMEDSURFACE(#9,350.,2.75,10.,4.25,.T.,.T.);",
      "#11", "'Surface','Surface3D'"));
  ASSERT_NO_FATAL_FAILURE(expectStandaloneParaboloid(
      withoutApex, {}, 1, 1.5, pi / 18, .5));
}

TEST(IfcCurveFamilies, TrimmedSmoothRevolutionKeepsExactAuthoredKnotEnd) {
  constexpr std::array<glm::dvec2, 3> controls{{{0, 0}, {1, 0}, {2, 1}}};
  // Adding the span to the first knot rounds to zero, outside the spline domain.
  const auto definitions =
      smoothRevolutionDefinition(controls, false, "-1.E8,-1.E-20");
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      sweptDegreeUnits + definitions +
          "#11=IFCRECTANGULARTRIMMEDSURFACE(#9,350.,-1.E8,10.,-1.E-20,.T.,.T.);",
      "#11", "'Surface','Surface3D'"));
  ASSERT_NO_FATAL_FAILURE(
      expectStandaloneParaboloid(model, {}, 1, 2, pi / 18));
}

TEST(IfcCurveFamilies, InvalidSmoothRevolutionLeavesValidSiblingUnchanged) {
  const std::string sibling =
      "#101=IFCCARTESIANPOINT((3.,0.)); #102=IFCCARTESIANPOINT((4.,0.));"
      "#103=IFCPOLYLINE((#101,#102)); #104=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#103);"
      "#105=IFCDIRECTION((0.,0.,1.)); #106=IFCSURFACEOFLINEAREXTRUSION(#104,$,#105,2.);";
  const auto reference = container::geometry::ifc::LoadFromStep(
      fixture(sibling, "#106", "'Surface','Surface3D'"));
  ASSERT_NO_FATAL_FAILURE(completeSweptMesh(reference));
  for (const auto &controls : std::array<std::array<glm::dvec2, 3>, 2>{{
           {{{-1, 0}, {1, 0}, {2, 1}}}, {{{0, 0}, {1, .25}, {2, 1}}},
       }}) {
    const auto definitions = smoothRevolutionDefinition(controls) + sibling +
        "#11=IFCRECTANGULARTRIMMEDSURFACE(#9,0.,2.,1.,4.25,.T.,.T.);";
    for (const std::string item : {"#9", "#11"}) {
      SCOPED_TRACE(item);
      const auto model = container::geometry::ifc::LoadFromStep(
          fixture(definitions, item + ",#106", "'Surface','Surface3D'"));
      EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
      ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
      EXPECT_EQ(model.importReport.diagnostics.front().entityId,
                item == "#9" ? 9u : 11u);
      EXPECT_FALSE(model.importReport.diagnostics.front().reason.empty());
      EXPECT_EQ(model.indices, reference.indices);
      ASSERT_EQ(model.meshRanges.size(), reference.meshRanges.size());
      ASSERT_EQ(model.vertices.size(), reference.vertices.size());
      for (size_t i = 0; i < model.vertices.size(); ++i) {
        EXPECT_EQ(model.vertices[i].position, reference.vertices[i].position);
        EXPECT_EQ(model.vertices[i].normal, reference.vertices[i].normal);
      }
    }
  }
}

TEST(IfcCurveFamilies, InvalidStandaloneSweepTrimLeavesOnlyFallbackSurface) {
  constexpr std::string_view definitions =
      "#2=IFCCARTESIANPOINT((1.,2.)); #3=IFCCARTESIANPOINT((4.,2.));"
      "#4=IFCPOLYLINE((#2,#3)); #5=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#4);"
      "#6=IFCDIRECTION((0.,0.,1.)); #7=IFCSURFACEOFLINEAREXTRUSION(#5,$,#6,2.);"
      "#9=IFCRECTANGULARTRIMMEDSURFACE(#7,0.25,1.5,0.75,-0.5,.T.,.F.);"
      "#10=IFCRECTANGULARTRIMMEDSURFACE(#7,0.5,0.,0.5,1.,.T.,.T.);";
  const auto reference = container::geometry::ifc::LoadFromStep(
      fixture(definitions, "#9", "'Surface','Surface3D'"));
  completeSweptMesh(reference);
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(definitions, "#10,#9", "'Surface','Surface3D'"));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  EXPECT_EQ(model.importReport.diagnostics.front().entityId, 10u);
  EXPECT_EQ(model.indices, reference.indices);
  ASSERT_EQ(model.vertices.size(), reference.vertices.size());
  for (size_t i = 0; i < model.vertices.size(); ++i) {
    EXPECT_EQ(model.vertices[i].position, reference.vertices[i].position);
    EXPECT_EQ(model.vertices[i].normal, reference.vertices[i].normal);
  }
  std::set<std::array<float, 3>> corners;
  for (const auto &vertex : model.vertices) {
    const auto p = vertex.position;
    EXPECT_EQ(p.y, 2.);
    EXPECT_GE(p.x, 1.75);
    EXPECT_LE(p.x, 3.25);
    EXPECT_GE(p.z, -1.);
    EXPECT_LE(p.z, 3.);
    EXPECT_GT(vertex.normal.y, .999);
    corners.insert({p.x, p.y, p.z});
  }
  for (float x : {1.75f, 3.25f})
    for (float z : {-1.f, 3.f})
      EXPECT_TRUE(corners.contains({x, 2.f, z}));
}

TEST(IfcCurveFamilies, RectangularSurfaceTrimsHaveLocalDomainsAndReverseSense) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
      "#4=IFCPLANE(#3); "
      "#5=IFCRECTANGULARTRIMMEDSURFACE(#4,3.,4.,1.,2.,.F.,.F.);"
      "#6=IFCCARTESIANPOINT((0.,0.)); #7=IFCCARTESIANPOINT((2.,2.));"
      "#8=IFCPOLYLINE((#6,#7)); #10=IFCPCURVE(#5,#8);"));
  complete(model);
  const auto p = vertices(model);
  EXPECT_EQ(p.front(), glm::dvec3(3, 4, 0));
  EXPECT_EQ(p.back(), glm::dvec3(1, 2, 0));
}

const std::string boundedPlane =
    "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$); "
    "#4=IFCPLANE(#3);"
    "#11=IFCCARTESIANPOINT((-2.,-2.)); #12=IFCCARTESIANPOINT((2.,-2.));"
    "#13=IFCCARTESIANPOINT((2.,2.)); #14=IFCCARTESIANPOINT((-2.,2.));"
    "#15=IFCPOLYLINE((#11,#12,#13,#14,#11));"
    "#21=IFCCARTESIANPOINT((-0.25,-0.25)); #22=IFCCARTESIANPOINT((-0.25,0.25));"
    "#23=IFCCARTESIANPOINT((0.25,0.25)); #24=IFCCARTESIANPOINT((0.25,-0.25));"
    "#25=IFCPOLYLINE((#21,#22,#23,#24,#21)); "
    "#5=IFCCURVEBOUNDEDPLANE(#4,#15,(#25));";
TEST(IfcCurveFamilies, BoundedPlanePcurvesRespectOuterAndHoleLoops) {
  for (bool hole : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(boundedPlane + "#30=IFCCARTESIANPOINT((-1.," +
                std::string(hole ? "0." : "1.") +
                "));"
                "#31=IFCCARTESIANPOINT((1.," +
                std::string(hole ? "0." : "1.") +
                "));"
                "#32=IFCPOLYLINE((#30,#31)); #10=IFCPCURVE(#5,#32);"));
    if (!hole)
      complete(model);
    else {
      EXPECT_TRUE(model.vertices.empty());
      EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    }
  }
}
TEST(IfcCurveFamilies, SmallSurfaceHolesDoNotAliasBetweenPcurveProbes) {
  std::string definitions = boundedPlane;
  const auto begin = definitions.find("#21="), end = definitions.find("#25=");
  definitions.replace(begin, end - begin,
                      "#21=IFCCARTESIANPOINT((0.101,-0.004)); "
                      "#22=IFCCARTESIANPOINT((0.101,0.004));"
                      "#23=IFCCARTESIANPOINT((0.109,0.004)); "
                      "#24=IFCCARTESIANPOINT((0.109,-0.004));");
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(definitions +
              "#30=IFCCARTESIANPOINT((-1.,0.)); #31=IFCCARTESIANPOINT((1.,0.));"
              "#32=IFCPOLYLINE((#30,#31)); #10=IFCPCURVE(#5,#32);"));
  EXPECT_TRUE(model.vertices.empty());
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
}

TEST(IfcCurveFamilies, BoundaryCurveFamiliesCanBoundAnotherSurface) {
  for (const std::string type : {"IFCBOUNDARYCURVE", "IFCOUTERBOUNDARYCURVE"}) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        boundedPlane +
        "#40=IFCPCURVE(#4,#15); #51=IFCCARTESIANPOINT((-2.,-2.,0.));"
        "#52=IFCCARTESIANPOINT((2.,-2.,0.)); #53=IFCCARTESIANPOINT((2.,2.,0.));"
        "#54=IFCCARTESIANPOINT((-2.,2.,0.)); "
        "#42=IFCPOLYLINE((#51,#52,#53,#54,#51));"
        "#43=IFCSURFACECURVE(#42,(#40),.PCURVE_S1.);"
        "#44=IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#43);"
        "#45=" +
        type +
        "((#44),.F.); #46=IFCCURVEBOUNDEDSURFACE(#4,(#45),.F.);"
        "#47=IFCCARTESIANPOINT((-1.,1.)); #48=IFCCARTESIANPOINT((1.,1.));"
        "#49=IFCPOLYLINE((#47,#48)); #10=IFCPCURVE(#46,#49);"));
    complete(model);
    EXPECT_NEAR(length(vertices(model)), 2, 1e-6);
  }
}
TEST(IfcCurveFamilies, ClosedSolidAndHollowSweepsHaveNoCapAtTheirSeam) {
  for (bool hollow : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
        "#4=IFCCIRCLE(#3,5.); #10=IFCSWEPTDISKSOLID(#4,0.5," +
        std::string(hollow ? "0.25" : "$") + ",$,$);"));
    for (const auto &d : model.importReport.diagnostics)
      ADD_FAILURE() << d.reason;
    ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(volume(model), 2 * pi * pi * 5 * (hollow ? .25 - .0625 : .25),
                .04);
    watertight(model);
  }
}
TEST(IfcCurveFamilies, SplineSweepsRetainSourceKnotDomainAndHollowWall) {
  for (bool trim : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        "#11=IFCCARTESIANPOINT((-2.,0.,0.)); "
        "#12=IFCCARTESIANPOINT((-1.,1.,1.));"
        "#13=IFCCARTESIANPOINT((1.,-1.,1.)); #14=IFCCARTESIANPOINT((2.,0.,0.));"
        "#4=IFCBSPLINECURVEWITHKNOTS(3,(#11,#12,#13,#14),.UNSPECIFIED.,.F.,.F.,"
        "(4,4),(2.,4.),.UNSPECIFIED.);"
        "#10=IFCSWEPTDISKSOLID(#4,0.1,0.05," +
        std::string(trim ? "2.5,3.5" : "$,$") + ");"));
    for (const auto &d : model.importReport.diagnostics)
      ADD_FAILURE() << d.reason;
    ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    const double a = trim ? .25 : 0, b = trim ? .75 : 1;
    double distance = 0;
    for (unsigned i = 0; i < 20000; ++i) {
      const double t = a + (b - a) * (i + .5) / 20000;
      const auto derivative = 3. * ((1 - t) * (1 - t) * glm::dvec3(1, 1, 1) +
                                    2 * t * (1 - t) * glm::dvec3(2, -2, 0) +
                                    t * t * glm::dvec3(1, 1, -1));
      distance += glm::length(derivative) * (b - a) / 20000;
    }
    const double expected = pi * .0075 * distance;
    EXPECT_NEAR(volume(model), expected, expected * .007);
    watertight(model);
  }
}
TEST(SweptDiskGeometry, ClosedNonplanarTransportAndSelfIntersectionChecks) {
  container::geometry::ParametricCurve c;
  c.domain = {{0, 2 * pi}};
  c.period = 2 * pi;
  c.point = [](double t) -> std::optional<glm::dvec3> {
    return glm::dvec3(3 * std::cos(t), 3 * std::sin(t), .5 * std::sin(2 * t));
  };
  c.derivative = [](double t) -> std::optional<glm::dvec3> {
    return glm::dvec3(-3 * std::sin(t), 3 * std::cos(t), std::cos(2 * t));
  };
  std::string error;
  const auto triangles =
      container::geometry::buildSweptDisk(c, 0, 2 * pi, .1, 0, .001, error);
  ASSERT_FALSE(triangles.empty()) << error;
  Model model;
  for (const auto &t : triangles)
    for (auto p : t) {
      model.indices.push_back(uint32_t(model.vertices.size()));
      decltype(model.vertices)::value_type vertex{};
      vertex.position = glm::vec3(p);
      model.vertices.push_back(vertex);
    }
  watertight(model);
  double distance = 0;
  for (unsigned i = 0; i < 20000; ++i)
    distance +=
        std::sqrt(9 + std::pow(std::cos(4 * pi * (i + .5) / 20000), 2)) * 2 *
        pi / 20000;
  EXPECT_NEAR(volume(model), pi * .01 * distance, pi * .01 * distance * .007);
  c.point = [](double t) -> std::optional<glm::dvec3> {
    return glm::dvec3(std::sin(t), std::sin(2 * t), 0);
  };
  c.derivative = [](double t) -> std::optional<glm::dvec3> {
    return glm::dvec3(std::cos(t), 2 * std::cos(2 * t), 0);
  };
  EXPECT_TRUE(
      container::geometry::buildSweptDisk(c, 0, 2 * pi, .01, 0, .001, error)
          .empty());
  EXPECT_NE(error.find("self-intersecting"), std::string::npos) << error;
}

TEST(IfcCurveFamilies, MiterSweepsHandleOpenHollowBendsAndClosedCorners) {
  for (bool closed : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        "#11=IFCCARTESIANPOINT((0.,0.,0.)); #12=IFCCARTESIANPOINT((3.,0.,0.));"
        "#13=IFCCARTESIANPOINT((3.,5.,0.)); #14=IFCCARTESIANPOINT((0.,5.,0.));"
        "#4=IFCPOLYLINE((#11,#12,#13" +
        std::string(closed ? ",#14,#11" : "") +
        "));"
        "#10=IFCSWEPTDISKSOLID(#4,0.25,0.1,$,$);"));
    for (const auto &d : model.importReport.diagnostics)
      ADD_FAILURE() << d.reason;
    ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(volume(model), pi * (.0625 - .01) * (closed ? 16 : 8), .012);
    watertight(model);
  }
}

TEST(IfcCurveFamilies, InvalidSweepsAndSurfaceTreesRejectAtomically) {
  const std::vector<std::string> definitions{
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
      "#4=IFCCIRCLE(#3,1.); #10=IFCSWEPTDISKSOLID(#4,2.,$,$,$);",
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((1.,0.,0.));"
      "#4=IFCCARTESIANPOINT((0.,0.1,0.)); #5=IFCPOLYLINE((#2,#3,#4));"
      "#10=IFCSWEPTDISKSOLID(#5,0.1,$,$,$);",
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((0.1,0.,0.));"
      "#4=IFCCARTESIANPOINT((0.1,0.1,0.)); #5=IFCPOLYLINE((#2,#3,#4));"
      "#10=IFCSWEPTDISKSOLID(#5,1.,$,$,$);",
      placement2D + "#4=IFCCIRCLE(#3,1.); "
                    "#5=IFCRECTANGULARTRIMMEDSURFACE(#5,0.,0.,1.,1.,.T.,.T.);"
                    "#10=IFCPCURVE(#5,#4);",
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
      "#4=IFCPLANE(#3); "
      "#5=IFCRECTANGULARTRIMMEDSURFACE(#4,0.,0.,1.,1.,.F.,.T.);"
      "#6=IFCCARTESIANPOINT((0.,0.)); #7=IFCCARTESIANPOINT((1.,1.));"
      "#8=IFCPOLYLINE((#6,#7)); #10=IFCPCURVE(#5,#8);"};
  for (const auto &d : definitions) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(d));
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_FALSE(model.importReport.diagnostics.front().reason.empty());
  }
}

TEST(ParametricCurve, PiecewiseSpeedIntegrationUsesTheCorrectSideOfJoins) {
  container::geometry::ParametricCurve c;
  c.domain = {{0, 2}};
  c.breaks = {1};
  c.point = [](double t) -> std::optional<glm::dvec3> {
    return t <= 1 ? glm::dvec3(3 * t, 0, 0) : glm::dvec3(3, 5 * (t - 1), 0);
  };
  c.derivative = [](double t) -> std::optional<glm::dvec3> {
    return t <= 1 ? glm::dvec3(3, 0, 0) : glm::dvec3(0, 5, 0);
  };
  const auto length = container::geometry::curveLength(c, 0, 2, 1e-10);
  ASSERT_TRUE(length);
  EXPECT_NEAR(*length, 8, 1e-10);
  const auto t = container::geometry::curveParameterAtDistance(c, 6, 0, 1e-9);
  ASSERT_TRUE(t);
  EXPECT_NEAR(*t, 1.6, 1e-9);
  EXPECT_EQ(container::geometry::curveParameterAtDistance(c, 8, 0, 1e-9), 2);
  EXPECT_EQ(container::geometry::curveParameterAtDistance(c, -8, 2, 1e-9), 0);
}

TEST(IfcCurveFamilies, RectangularSurfaceTrimsCarryDegreeUnitsAcrossASeam) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
      "#4=IFCCYLINDRICALSURFACE(#3,2.); "
      "#5=IFCRECTANGULARTRIMMEDSURFACE(#4,350.,0.,10.,1.,.T.,.T.);"
      "#6=IFCCARTESIANPOINT((0.,0.)); #7=IFCCARTESIANPOINT((20.,1.));"
      "#8=IFCPOLYLINE((#6,#7)); #10=IFCPCURVE(#5,#8);"
      "#30=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.);"
      "#31=IFCMEASUREWITHUNIT(IFCPLANEANGLEMEASURE(0.017453292519943295),#30);"
      "#32=IFCCONVERSIONBASEDUNIT($,.PLANEANGLEUNIT.,'degree',#31);"
      "#33=IFCUNITASSIGNMENT((#1,#32)); "
      "#34=IFCPROJECT('project',$,$,$,$,$,$,$,#33);"));
  complete(model);
  const auto p = vertices(model);
  for (auto v : p) {
    const double a = (350 + 20 * v.z) * pi / 180;
    EXPECT_NEAR(v.x, 2 * std::cos(a), 1e-6);
    EXPECT_NEAR(v.y, 2 * std::sin(a), 1e-6);
  }
  EXPECT_NEAR(length(p), std::sqrt(1 + 4 * pi * pi / 81), .002);
}

TEST(IfcCurveFamilies, PolygonalFilletsPreserveVolumeAndHollowTopology) {
  for (bool indexed : {false, true})
    for (bool trim : {false, true}) {
      const std::string path =
          indexed
              ? "#4=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(3.,0.,0.),(3.,5.,0.)))"
                ";"
                "#5=IFCINDEXEDPOLYCURVE(#4,$,.F.);"
              : "#2=IFCCARTESIANPOINT((0.,0.,0.)); "
                "#3=IFCCARTESIANPOINT((3.,0.,0.));"
                "#4=IFCCARTESIANPOINT((3.,5.,0.)); #5=IFCPOLYLINE((#2,#3,#4));";
      auto model = container::geometry::ifc::LoadFromStep(
          fixture(path + "#10=IFCSWEPTDISKSOLIDPOLYGONAL(#5,0.1,0.05," +
                  std::string(trim ? "0.5,1.5" : "$,$") + ",0.5);"));
      for (const auto &d : model.importReport.diagnostics)
        ADD_FAILURE() << d.reason;
      ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
      const double distance = (trim ? 3 : 7) + pi / 4;
      EXPECT_NEAR(volume(model), pi * .0075 * distance, .0015);
      watertight(model);
    }
}

TEST(IfcCurveFamilies, ClosedPolygonalFilletsJoinAcrossTheirSourceSeam) {
  auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((6.,0.,0.));"
      "#4=IFCCARTESIANPOINT((6.,4.,0.)); #5=IFCCARTESIANPOINT((0.,4.,0.));"
      "#6=IFCPOLYLINE((#2,#3,#4,#5,#2));"
      "#10=IFCSWEPTDISKSOLIDPOLYGONAL(#6,0.1,$,$,$,0.5);"));
  for (const auto &d : model.importReport.diagnostics)
    ADD_FAILURE() << d.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_NEAR(volume(model), pi * .01 * (16 + pi), .003);
  watertight(model);
}

TEST(IfcCurveFamilies, AdjacentPolygonalFilletsMayMeetWithoutAStraightSpan) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((2.,0.,0.));"
      "#4=IFCCARTESIANPOINT((2.,2.,0.)); #5=IFCCARTESIANPOINT((0.,2.,0.));"
      "#6=IFCPOLYLINE((#2,#3,#4,#5,#2));"
      "#10=IFCSWEPTDISKSOLIDPOLYGONAL(#6,0.1,$,$,$,1.);"));
  for (const auto &d : model.importReport.diagnostics)
    ADD_FAILURE() << d.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_NEAR(volume(model), 2 * pi * pi * .01, .002);
  watertight(model);
}

TEST(IfcCurveFamilies, PolygonalSweepWithoutFilletsRetainsItsMiter) {
  auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((3.,0.,0.));"
      "#4=IFCCARTESIANPOINT((3.,5.,0.)); #5=IFCPOLYLINE((#2,#3,#4));"
      "#10=IFCSWEPTDISKSOLIDPOLYGONAL(#5,0.1,$,$,$,$);"));
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_NEAR(volume(model), pi * .01 * 8, .002);
  watertight(model);
}

TEST(IfcCurveFamilies, MalformedPolygonalFilletsRejectBeforeBufferMutation) {
  for (const std::string path :
       {"#4=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(3.,0.,0.),(3.,5.,0.)));"
        "#5=IFCINDEXEDPOLYCURVE(#4,(IFCLINEINDEX((1,2,3))),.F.);",
        "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((0.2,0.,0.));"
        "#4=IFCCARTESIANPOINT((0.2,5.,0.)); #5=IFCPOLYLINE((#2,#3,#4));"}) {
    auto model = container::geometry::ifc::LoadFromStep(
        fixture(path + "#10=IFCSWEPTDISKSOLIDPOLYGONAL(#5,0.1,$,$,$,0.5);"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    ASSERT_FALSE(model.importReport.diagnostics.empty());
    EXPECT_NE(model.importReport.diagnostics.front().reason.find("directrix"),
              std::string::npos);
  }
  std::string error;
  EXPECT_FALSE(container::geometry::makeFilletedPolyline(
      {{0, 0, 0}, {2, 0, 0}, {0, 0, 0}}, .2, error));
}

const std::string sectionedFixture =
    "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((10.,0.,0.));"
    "#4=IFCPOLYLINE((#2,#3));"
    "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(0.),$,$,$,#4);"
    "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(10.),$,$,$,#4);"
    "#22=IFCAXIS2PLACEMENTLINEAR(#20,$,$); "
    "#23=IFCAXIS2PLACEMENTLINEAR(#21,$,$);"
    "#24=IFCCARTESIANPOINT((-2.,0.)); #25=IFCCARTESIANPOINT((-4.,2.));"
    "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,2.),(0.,0.),('left','crown','"
    "right'),#24);"
    "#31=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(4.,4.),(0.,0.),('left','crown','"
    "right'),#25);"
    "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#31));"
    "#41=IFCCARTESIANPOINT((0.,0.25)); #42=IFCCARTESIANPOINT((1.,0.25));"
    "#43=IFCPOLYLINE((#41,#42)); #10=IFCPCURVE(#40,#43);";

TEST(IfcCurveFamilies,
     SectionedSurfacesInterpolateProfilesInPlacedCrossPlanes) {
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(sectionedFixture));
  complete(model);
  const auto p = vertices(model);
  ASSERT_FALSE(p.empty());
  EXPECT_EQ(p.front(), glm::dvec3(0, -1, 0));
  EXPECT_EQ(p.back(), glm::dvec3(10, -2, 2));
  EXPECT_NEAR(length(p), std::sqrt(105.), 1e-5);
  for (auto q : p) {
    EXPECT_NEAR(q.y, -1 - q.x / 10, 1e-6);
    EXPECT_NEAR(q.z, q.x / 5, 1e-6);
  }
}

TEST(IfcCurveFamilies, SectionedSurfacesFollowCurvedDirectrices) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
      "#4=IFCCIRCLE(#3,5.);"
      "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(0.),$,$,$,#4);"
      "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(1.5707963267948966),$"
      ",$,$,#4);"
      "#22=IFCAXIS2PLACEMENTLINEAR(#20,$,$); "
      "#23=IFCAXIS2PLACEMENTLINEAR(#21,$,$);"
      "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.),(0.),$,$);"
      "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#30));"
      "#41=IFCCARTESIANPOINT((0.,0.5)); "
      "#42=IFCCARTESIANPOINT((1.5707963267948966,0.5));"
      "#43=IFCPOLYLINE((#41,#42)); #10=IFCPCURVE(#40,#43);"));
  complete(model);
  for (auto p : vertices(model)) {
    EXPECT_NEAR(glm::length(glm::dvec2(p)), 4, 1e-6);
    EXPECT_NEAR(p.z, 0, 1e-6);
  }
  EXPECT_NEAR(length(vertices(model)), 2 * pi, .003);
}

TEST(IfcCurveFamilies, OpenCrossProfilesUseHorizontalAndSlopingWidths) {
  for (bool horizontal : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
        "#4=IFCDIRECTION((0.,0.,1.));"
        "#5=IFCOPENCROSSPROFILEDEF(.CURVE.,$,." +
        std::string(horizontal ? "T" : "F") +
        ".,(2.),(0.5235987755982988),$,$);"
        "#6=IFCSURFACEOFLINEAREXTRUSION(#5,#3,#4,1.);"
        "#7=IFCCARTESIANPOINT((0.,0.)); #8=IFCCARTESIANPOINT((1.,0.));"
        "#9=IFCPOLYLINE((#7,#8)); #10=IFCPCURVE(#6,#9);"));
    complete(model);
    const auto p = vertices(model);
    EXPECT_NEAR(p.back().x, horizontal ? 2 : std::sqrt(3.), 1e-6);
    EXPECT_NEAR(p.back().y, horizontal ? 2 / std::sqrt(3.) : 1, 1e-6);
    EXPECT_NEAR(length(p), horizontal ? 4 / std::sqrt(3.) : 2, 1e-6);
  }
}

TEST(IfcCurveFamilies, MalformedSectionedSurfacesReportTheirSource) {
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"IFCLENGTHMEASURE(10.)", "IFCLENGTHMEASURE(0.)"},
           {"('left','crown','right'),#25", "('left','branch','right'),#25"},
           {"#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#31))",
            "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30))"},
           {"(4.,4.),(0.,0.)", "(4.,4.),(0.)"},
           {"IFCLENGTHMEASURE(10.),$,$,$", "IFCLENGTHMEASURE(10.),0.,$,$"}}) {
    auto definitions = sectionedFixture;
    const auto at = definitions.find(from);
    ASSERT_NE(at, std::string::npos);
    definitions.replace(at, from.size(), to);
    auto model = container::geometry::ifc::LoadFromStep(fixture(definitions));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    ASSERT_FALSE(model.importReport.diagnostics.empty());
  }
}

TEST(IfcCurveFamilies, SectionedSurfacesAlsoProduceDrawableOpenMeshes) {
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(sectionedFixture, "#40"));
  for (const auto &d : model.importReport.diagnostics)
    ADD_FAILURE() << d.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_TRUE(model.nativeCurveRanges.empty());
  EXPECT_FALSE(model.indices.empty());
  double area = 0;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    const glm::dvec3 a(model.vertices[model.indices[i]].position),
        b(model.vertices[model.indices[i + 1]].position),
        c(model.vertices[model.indices[i + 2]].position);
    const auto normal = glm::cross(b - a, c - a);
    area += glm::length(normal) * .5;
    EXPECT_GT(normal.z, 0);
  }
  EXPECT_NEAR(area, 6 * std::sqrt(104.), 1e-5);
  // A planar trapezoid needs no subdivision for tangential parameter warping.
  EXPECT_EQ(model.indices.size(), 12u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements.front().sourceId, "#900003");
  EXPECT_EQ(model.elements.front().guid, "curve-families");
}

TEST(IfcCurveFamilies,
     CurvedSectionedMeshesHaveAnalyticAreaAndSharedInteriorEdges) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
      "#4=IFCCIRCLE(#3,5.);"
      "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(0.),$,$,$,#4);"
      "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(1.5707963267948966),$"
      ",$,$,#4);"
      "#22=IFCAXIS2PLACEMENTLINEAR(#20,$,$); "
      "#23=IFCAXIS2PLACEMENTLINEAR(#21,$,$);"
      "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.),(0.),$,$);"
      "#10=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#30));"));
  for (const auto &d : model.importReport.diagnostics)
    ADD_FAILURE() << d.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  double area = 0;
  std::map<std::array<float, 3>, size_t> points;
  std::map<std::pair<size_t, size_t>, std::pair<size_t, int>> edges;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    std::array<size_t, 3> triangle;
    std::array<glm::dvec3, 3> positions;
    for (size_t k = 0; k < 3; ++k) {
      const auto p = model.vertices[model.indices[i + k]].position;
      triangle[k] =
          points.try_emplace({p.x, p.y, p.z}, points.size()).first->second;
      positions[k] = p;
      EXPECT_GE(glm::length(glm::dvec2(p)), 3 - 1e-6);
      EXPECT_LE(glm::length(glm::dvec2(p)), 5 + 1e-6);
    }
    const auto normal =
        glm::cross(positions[1] - positions[0], positions[2] - positions[0]);
    EXPECT_GT(normal.z, 0);
    area += glm::length(normal) * .5;
    for (size_t k = 0; k < 3; ++k) {
      const auto a = triangle[k], b = triangle[(k + 1) % 3];
      auto &edge = edges[{std::min(a, b), std::max(a, b)}];
      ++edge.first;
      edge.second += a < b ? 1 : -1;
    }
  }
  EXPECT_NEAR(area, 4 * pi, .004);
  for (const auto &[_, edge] : edges) {
    EXPECT_LE(edge.first, 2u);
    if (edge.first == 2)
      EXPECT_EQ(edge.second, 0);
  }
}

const std::string branchingSectionedFixture =
    "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((10.,0.,0.));"
    "#4=IFCPOLYLINE((#2,#3));"
    "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(0.),$,$,$,#4);"
    "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(10.),$,$,$,#4);"
    "#22=IFCAXIS2PLACEMENTLINEAR(#20,$,$); "
    "#23=IFCAXIS2PLACEMENTLINEAR(#21,$,$);"
    "#24=IFCCARTESIANPOINT((-2.,0.));"
    "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,2.),(0.,0.),('left','branch',"
    "'right'),#24);"
    "#31=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(1.,2.,1.),(0.,0.,0.),('left','"
    "branch','branch','right'),#24);"
    "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#31));";

void replace(std::string &value, const std::string &from,
             const std::string &to) {
  const auto at = value.find(from);
  ASSERT_NE(at, std::string::npos) << from;
  value.replace(at, from.size(), to);
}
void exportSectionedFixture(const char *name, const std::string &source) {
  if (const auto *root = std::getenv("CONTAINER_IFC_SECTIONED_FIXTURE_ROOT")) {
    std::filesystem::create_directories(root);
    std::ofstream stream(std::filesystem::path(root) /
                         (std::string(name) + ".ifc"));
    stream << source;
    ASSERT_TRUE(stream.good());
  }
}
double checkSectionedMesh(const Model &model, glm::dvec3 up = {0, 0, 1}) {
  for (const auto &d : model.importReport.diagnostics)
    ADD_FAILURE() << d.reason;
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_FALSE(model.indices.empty());
  EXPECT_TRUE(model.nativeCurveRanges.empty());
  using Key = std::array<float, 3>;
  std::map<std::pair<Key, Key>, std::pair<unsigned, int>> edges;
  std::set<Key> used;
  double area = 0;
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    std::array<glm::dvec3, 3> p;
    std::array<Key, 3> keys;
    for (unsigned j = 0; j < 3; ++j) {
      const auto v = model.vertices.at(model.indices.at(i + j)).position;
      p[j] = v;
      keys[j] = {v.x, v.y, v.z};
      used.insert(keys[j]);
    }
    const auto normal = glm::cross(p[1] - p[0], p[2] - p[0]);
    EXPECT_GT(glm::dot(normal, up), 0);
    area += glm::length(normal) / 2;
    for (unsigned j = 0; j < 3; ++j) {
      const auto a = keys[j], b = keys[(j + 1) % 3];
      EXPECT_NE(a, b);
      auto &use = edges[std::minmax(a, b)];
      ++use.first;
      use.second += a < b ? 1 : -1;
    }
  }
  std::map<Key, std::vector<Key>> boundary;
  for (const auto &[edge, use] : edges) {
    EXPECT_LE(use.first, 2u);
    if (use.first == 2)
      EXPECT_EQ(use.second, 0);
    else if (use.first == 1) {
      boundary[edge.first].push_back(edge.second);
      boundary[edge.second].push_back(edge.first);
    }
  }
  // These fixtures are topological disks. Interior cracks and T-junctions
  // create extra boundary uses, and a disconnected patch changes Euler count.
  EXPECT_EQ(static_cast<int64_t>(used.size()) -
                static_cast<int64_t>(edges.size()) +
                static_cast<int64_t>(model.indices.size() / 3),
            1);
  EXPECT_FALSE(boundary.empty());
  for (const auto &[_, neighbors] : boundary)
    EXPECT_EQ(neighbors.size(), 2u);
  if (!boundary.empty()) {
    std::set<Key> visited;
    std::vector<Key> pending{boundary.begin()->first};
    while (!pending.empty()) {
      const auto p = pending.back();
      pending.pop_back();
      if (visited.insert(p).second)
        for (const auto &q : boundary.at(p))
          pending.push_back(q);
    }
    EXPECT_EQ(visited.size(), boundary.size());
  }
  return area;
}

const std::string sectionedStraightBase =
    "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((10.,0.,0.));"
    "#4=IFCPOLYLINE((#2,#3));"
    "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(0.),$,$,$,#4);"
    "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(10.),$,$,$,#4);"
    "#22=IFCAXIS2PLACEMENTLINEAR(#20,$,$); "
    "#23=IFCAXIS2PLACEMENTLINEAR(#21,$,$);"
    "#24=IFCCARTESIANPOINT((-2.,0.));";

const std::string guideSectionedFixture =
    sectionedStraightBase +
    "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,2.),(0.,0.),"
    "('left','crown','right'),#24);"
    "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#30));";

struct GuideSectionFixture {
  std::string definitions = guideSectionedFixture;
  uint32_t next = 100;

  std::string add(const std::string &value) {
    const auto ref = "#" + std::to_string(next++);
    definitions += ref + "=" + value + ";";
    return ref;
  }

  std::string point(double x, double y, double z) {
    return add("IFCCARTESIANPOINT((" + scalar(x) + "," + scalar(y) + "," +
               scalar(z) + "))");
  }

  struct OffsetSample {
    double station, lateral, vertical;
  };

  std::string guide(std::string_view tag, std::string_view basis,
                    std::initializer_list<OffsetSample> offsets) {
    std::string positions;
    for (const auto &[station, lateral, vertical] : offsets) {
      if (!positions.empty())
        positions += ',';
      positions += add("IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(" +
                       scalar(station) + ")," + scalar(lateral) + "," +
                       scalar(vertical) + ",$," + std::string(basis) + ")");
    }
    return add("IFCOFFSETCURVEBYDISTANCES(" + std::string(basis) + ",(" +
               positions + "),'" + std::string(tag) + "')");
  }

  void product(std::string_view guid, const std::string &axisItems = {},
               std::string_view bodyItem = "#40") {
    auto reps = add("IFCSHAPEREPRESENTATION($,'Body','SectionedSurface',(" +
                    std::string(bodyItem) + "))");
    if (!axisItems.empty())
      reps +=
          "," + add("IFCSHAPEREPRESENTATION($,'Axis','GeometricCurveSet',(" +
                    axisItems + "))");
    const auto pds = add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + reps + "))");
    add("IFCBUILDINGELEMENTPROXY('" + std::string(guid) + "',$,'Road',$,$,$," +
        pds + ",$,$)");
  }

  Model load() const {
    return container::geometry::ifc::LoadFromStep(
        "ISO-10303-21; DATA; #1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);" +
        definitions + "ENDSEC; END-ISO-10303-21;");
  }

private:
  static std::string scalar(double value) {
    std::ostringstream text;
    text.precision(17);
    text << value;
    return text.str();
  }
};

// Axis curves and Body triangles occupy the same model buffers. Use the
// product's draw ranges so a valid guide alone cannot satisfy the mesh test.
Model sectionedProductMesh(const Model &model, std::string_view guid) {
  Model body;
  body.importReport = model.importReport;
  body.vertices = model.vertices;
  for (const auto &element : model.elements) {
    if (element.guid != guid ||
        element.geometryKind != container::geometry::dotbim::GeometryKind::Mesh)
      continue;
    for (const auto &range : model.meshRanges) {
      if (range.meshId != element.meshId)
        continue;
      for (size_t i = range.firstIndex;
           i < size_t(range.firstIndex) + range.indexCount; ++i) {
        const auto index = model.indices.at(i);
        auto &vertex = body.vertices.at(index);
        // Undo the renderer's IFC Z-up to glTF Y-up conversion so these
        // bounds remain in the authored axes, including mapped translations.
        const auto p =
            element.transform * glm::vec4(model.vertices.at(index).position, 1);
        vertex.position = {p.x, -p.z, p.y};
        body.indices.push_back(index);
      }
    }
  }
  return body;
}

double maximumBodyHeight(const Model &body) {
  double height = -std::numeric_limits<double>::infinity();
  for (uint32_t i : body.indices)
    height = std::max(height, double(body.vertices.at(i).position.z));
  return height;
}

TEST(IfcCurveFamilies, IndexedSectionTagListFollowsProfileTraversal) {
  GuideSectionFixture step;
  step.definitions =
      sectionedStraightBase +
      "#25=IFCCARTESIANPOINTLIST2D(((0.,0.),(2.,0.),(-2.,0.),(99.,99.)),"
      "('crown','right','left','unused'));"
      "#26=IFCINDEXEDPOLYCURVE(#25,(IFCLINEINDEX((3,1)),IFCLINEINDEX((1,2))),."
      "F.);"
      "#27=IFCCARTESIANPOINTLIST2D(((-2.,0.),(0.,0.),(2.,0.)),"
      "('left','crown','right'));"
      "#28=IFCINDEXEDPOLYCURVE(#27,$,.F.);"
      "#30=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#26);"
      "#31=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#28);"
      "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#31));"
      "#45=IFCCARTESIANPOINT((0.,0.5)); #46=IFCCARTESIANPOINT((1.,0.5));"
      "#47=IFCPOLYLINE((#45,#46)); #48=IFCPCURVE(#40,#47);";
  const auto left =
      step.guide("left", "#4", {{0, -2, 0}, {4, -2, 3}, {10, -2, 0}});
  const auto crown =
      step.guide("crown", "#4", {{0, 0, 0}, {4, 0, 3}, {10, 0, 0}});
  const auto right =
      step.guide("right", "#4", {{0, 2, 0}, {4, 2, 3}, {10, 2, 0}});
  const auto unused = step.guide("unused", "#4", {{0, 8, 7}});
  const auto axis = step.add("IFCGEOMETRICCURVESET((" + left + "," + crown +
                             "," + right + "," + unused + ",#48))");
  step.product("indexed-tag-roof", axis);
  const auto model = step.load();
  const auto body = sectionedProductMesh(model, "indexed-tag-roof");
  EXPECT_NEAR(checkSectionedMesh(body), 4 * (5 + std::sqrt(45.)), 1e-5);
  EXPECT_NEAR(maximumBodyHeight(body), 3., 1e-6);
  for (float y : {-2.f, 0.f, 2.f}) {
    EXPECT_TRUE(std::ranges::any_of(body.indices, [&](uint32_t index) {
      return body.vertices.at(index).position == glm::vec3(4, y, 3);
    }));
  }

  size_t crownCurves = 0;
  for (const auto &range : model.nativeCurveRanges) {
    std::vector<glm::dvec3> points;
    for (size_t j = 0; j < range.indexCount; ++j)
      points.emplace_back(
          model.vertices.at(model.indices.at(range.firstIndex + j)).position);
    if (points.empty() || points.front() != glm::dvec3(0) ||
        points.back() != glm::dvec3(10, 0, 0))
      continue;
    ++crownCurves;
    ASSERT_EQ(points.size() % 2, 0u);
    EXPECT_EQ(std::ranges::count(points, glm::dvec3(4, 0, 3)), 2);
    EXPECT_NEAR(length(points), 5 + std::sqrt(45.), 1e-5);
  }
  EXPECT_EQ(crownCurves, 2u) << "Expected the guide and its lifted pcurve";
}

TEST(IfcCurveFamilies, IndexedSectionTagListKeepsUnboundOrdinalSlot) {
  GuideSectionFixture step;
  step.definitions = sectionedStraightBase +
                     "#25=IFCCARTESIANPOINTLIST2D(((-2.,0.),(0.5,0.),(2.,0.)),("
                     "'left','','right'));"
                     "#26=IFCINDEXEDPOLYCURVE(#25,$,.F.);"
                     "#30=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#26);"
                     "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#30));";
  const auto guide =
      step.guide("left", "#4", {{0, -2, 0}, {4, -3, 0}, {10, -2, 0}});
  const auto axis = step.add("IFCGEOMETRICCURVESET((" + guide + "))");
  step.product("untagged-ordinal-slot", axis);
  const auto body = sectionedProductMesh(step.load(), "untagged-ordinal-slot");
  // The moved left edge adds a 10 m by 1 m triangular wedge. The unnamed
  // center point remains at its authored lateral coordinate at the same row.
  EXPECT_NEAR(checkSectionedMesh(body), 45., 1e-5);
  for (float y : {-3.f, .5f, 2.f}) {
    EXPECT_TRUE(std::ranges::any_of(body.indices, [&](uint32_t index) {
      return body.vertices.at(index).position == glm::vec3(4, y, 0);
    }));
  }
}

TEST(IfcCurveFamilies,
     IndexedSectionTagListKeepsMirroredDerivedMillimetreTags) {
  GuideSectionFixture step;
  step.definitions =
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((10000.,0.,0.));"
      "#4=IFCPOLYLINE((#2,#3));"
      "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(0.),$,$,$,#4);"
      "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(10000.),$,$,$,#4);"
      "#22=IFCAXIS2PLACEMENTLINEAR(#20,$,$); "
      "#23=IFCAXIS2PLACEMENTLINEAR(#21,$,$);"
      "#25=IFCCARTESIANPOINTLIST2D(((0.,0.),(2000.,0.),(-2000.,0.)),"
      "('crown','edge-b','edge-a'));"
      "#26=IFCINDEXEDPOLYCURVE(#25,(IFCLINEINDEX((3,1,2))),.F.);"
      "#30=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#26);"
      "#31=IFCCARTESIANPOINT((500.,750.));"
      "#32=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#31,1.5,0.5);"
      "#33=IFCDERIVEDPROFILEDEF(.CURVE.,$,#30,#32,$);"
      "#34=IFCMIRROREDPROFILEDEF(.CURVE.,$,#33,*,$);"
      "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#34,#34));"
      "#45=IFCCARTESIANPOINT((0.,0.)); #46=IFCCARTESIANPOINT((1.,0.));"
      "#47=IFCPOLYLINE((#45,#46)); #48=IFCPCURVE(#40,#47);";
  const auto guide = [&](std::string_view tag, double y) {
    return step.guide(tag, "#4",
                      {{0, y, 750}, {4000, y, 2750}, {10000, y, 750}});
  };
  const auto a = guide("edge-a", 2500), crown = guide("crown", -500),
             b = guide("edge-b", -3500);
  const auto axis =
      step.add("IFCGEOMETRICCURVESET((" + a + "," + crown + "," + b + ",#48))");
  step.product("mirrored-indexed-tags", axis);
  const auto model = container::geometry::ifc::LoadFromStep(
      "ISO-10303-21; DATA; #1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);" +
      step.definitions + "ENDSEC; END-ISO-10303-21;");
  const auto body = sectionedProductMesh(model, "mirrored-indexed-tags");
  const double railLength = std::sqrt(20.) + std::sqrt(40.);
  EXPECT_NEAR(checkSectionedMesh(body, {0, 0, -1}), 6 * railLength, 1e-4);
  EXPECT_NEAR(maximumBodyHeight(body), 2.75, 1e-6);
  for (double y : {2.5, -.5, -3.5}) {
    EXPECT_TRUE(std::ranges::any_of(body.indices, [&](uint32_t index) {
      return glm::length(glm::dvec3(body.vertices.at(index).position) -
                         glm::dvec3(4, y, 2.75)) < 1e-5;
    }));
  }
  // V=0 follows the first traversed point, now on the reflected side.
  size_t firstRails = 0;
  for (const auto &element : model.elements) {
    const auto points = worldCurveEndpoints(model, element);
    if (points.empty() ||
        glm::length(points.front() - glm::dvec3(0, .75, -2.5)) > 1e-6 ||
        glm::length(points.back() - glm::dvec3(10, .75, -2.5)) > 1e-6)
      continue;
    ++firstRails;
    ASSERT_EQ(points.size() % 2, 0u);
    EXPECT_NEAR(length(points), railLength, 1e-5);
  }
  EXPECT_EQ(firstRails, 2u) << "Expected the reflected guide and V=0 pcurve";
}

TEST(IfcCurveFamilies, IndexedSectionTagListRejectsInvalidOrMixedRows) {
  for (const bool wrongLength : {false, true}) {
    SCOPED_TRACE(wrongLength);
    GuideSectionFixture step;
    step.definitions =
        sectionedStraightBase + std::string("#25=IFCCARTESIANPOINTLIST2D(") +
        (wrongLength ? "((-2.,0.),(0.,0.),(2.,0.),(99.,99.)),"
                     : "((-2.,0.),(0.,0.),(2.,0.)),") +
        "('left','crown','right'));"
        "#26=IFCINDEXEDPOLYCURVE(#25,(IFCLINEINDEX((1,2,3))),.F.);"
        "#27=IFCCARTESIANPOINTLIST2D(((-2.,0.),(0.,0.),(2.,0.)),$);"
        "#28=IFCINDEXEDPOLYCURVE(#27,$,.F.);"
        "#30=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#26);"
        "#31=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#28);"
        "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30," +
        (wrongLength ? "#30" : "#31") +
        "));"
        "#41=IFCSECTIONEDSURFACE(#4,(#22,#23),(#31,#31));";
    step.product("invalid-indexed-tags");
    step.product("untagged-fallback", {}, "#41");
    const auto model = step.load();
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
    EXPECT_TRUE(
        sectionedProductMesh(model, "invalid-indexed-tags").indices.empty());
    EXPECT_FALSE(
        sectionedProductMesh(model, "untagged-fallback").indices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    const auto &diagnostic = model.importReport.diagnostics.front();
    EXPECT_EQ(diagnostic.entityId, 40u);
    EXPECT_EQ(diagnostic.productGuid, "invalid-indexed-tags");
    EXPECT_TRUE(diagnostic.reason.find("tag") != std::string::npos ||
                diagnostic.reason.find("TagList") != std::string::npos)
        << diagnostic.reason;
  }
}

TEST(IfcCurveFamilies, IndexedSectionTagListGuidesPreserveAnalyticArc) {
  const double root2 = std::sqrt(2.);
  for (const double rise : {0., .5}) {
    SCOPED_TRACE(rise);
    GuideSectionFixture step;
    step.definitions =
        sectionedStraightBase +
        "#25=IFCCARTESIANPOINTLIST2D(((2.,0.),"
        "(1.4142135623730951,1.4142135623730951),(0.,2.)),"
        "('start','on-arc','end'));"
        "#26=IFCINDEXEDPOLYCURVE(#25,(IFCARCINDEX((1,2,3))),.F.);"
        "#30=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#26);"
        "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#30));"
        "#45=IFCCARTESIANPOINT((0.4,0.)); #46=IFCCARTESIANPOINT((0.4,1.));"
        "#47=IFCPOLYLINE((#45,#46)); #48=IFCPCURVE(#40,#47);"
        "#49=IFCCARTESIANPOINT((0.4,0.25)); #50=IFCCARTESIANPOINT((0.4,0.75));"
        "#51=IFCPOLYLINE((#49,#50)); #52=IFCPCURVE(#40,#51);";
    const auto guide = step.guide(
        "on-arc", "#4",
        {{0, root2, root2}, {4, root2, root2 + rise}, {10, root2, root2}});
    const auto axis = step.add("IFCGEOMETRICCURVESET((" + guide + ",#48,#52))");
    step.product("guided-indexed-arc", axis);
    const auto model = step.load();
    const auto body = sectionedProductMesh(model, "guided-indexed-arc");
    const double area = checkSectionedMesh(body, {0, 0, -1});
    if (rise == 0.) {
      EXPECT_NEAR(area, 10 * pi, .02);
      for (uint32_t index : body.indices) {
        const auto p = glm::dvec3(body.vertices.at(index).position);
        EXPECT_NEAR(std::hypot(p.y, p.z), 2., 1e-6);
      }
    }
    EXPECT_TRUE(std::ranges::any_of(body.indices, [&](uint32_t index) {
      const auto p = glm::dvec3(body.vertices.at(index).position);
      return glm::length(p - glm::dvec3(4, root2, root2 + rise)) < 1e-6;
    }));

    const auto point = [&](double v) {
      return glm::dvec3(4, 2 * std::cos(v * pi / 2),
                        2 * std::sin(v * pi / 2) + .5 * rise);
    };
    unsigned fullArc = 0, interiorArc = 0;
    for (const auto &range : model.nativeCurveRanges) {
      std::vector<glm::dvec3> points;
      for (size_t j = 0; j < range.indexCount; ++j)
        points.emplace_back(
            model.vertices.at(model.indices.at(range.firstIndex + j)).position);
      if (points.empty())
        continue;
      if (glm::length(points.front() - glm::dvec3(4, 2, 0)) < 1e-6 &&
          glm::length(points.back() - glm::dvec3(4, 0, 2)) < 1e-6) {
        ++fullArc;
        if (rise == 0.) {
          ASSERT_EQ(points.size() % 2, 0u);
          EXPECT_NEAR(length(points), pi, .002);
        }
      }
      if (glm::length(points.front() - point(.25)) < 1e-6 &&
          glm::length(points.back() - point(.75)) < 1e-6)
        ++interiorArc;
    }
    EXPECT_EQ(fullArc, 1u);
    EXPECT_EQ(interiorArc, 1u) << "The guide must add displacement to the arc";
  }
}

TEST(IfcCurveFamilies, SectionedGuidesKeepIndependentBasisAndIntermediateRow) {
  for (const bool independent : {false, true}) {
    SCOPED_TRACE(independent);
    GuideSectionFixture step;
    std::string basis = "#4";
    if (independent) {
      // Source guide parameter decreases across the surface and has a
      // different station origin and scale from the primary directrix.
      const auto a = step.point(15, 0, 0), b = step.point(-5, 0, 0);
      basis = step.add("IFCPOLYLINE((" + a + "," + b + "))");
    }
    std::string guides;
    for (const auto &[tag, lateral] :
         std::array<std::pair<const char *, double>, 3>{
             {{"left", -2}, {"crown", 0}, {"right", 2}}}) {
      const double signedOffset = independent ? -lateral : lateral;
      const auto guide = independent ? step.guide(tag, basis,
                                                  {{5, signedOffset, 0},
                                                   {11, signedOffset, 3},
                                                   {15, signedOffset, 0}})
                                     : step.guide(tag, basis,
                                                  {{0, signedOffset, 0},
                                                   {4, signedOffset, 3},
                                                   {10, signedOffset, 0}});
      if (!guides.empty())
        guides += ',';
      guides += guide;
    }
    const auto geometricSet =
        step.add("IFCGEOMETRICCURVESET((" + guides + "))");
    step.definitions +=
        "#45=IFCCARTESIANPOINT((0.4,0.)); #46=IFCCARTESIANPOINT((0.4,1.));"
        "#47=IFCPOLYLINE((#45,#46)); #48=IFCPCURVE(#40,#47);"
        "#49=IFCCARTESIANPOINT((0.,0.5)); #50=IFCCARTESIANPOINT((1.,0.5));"
        "#51=IFCPOLYLINE((#49,#50)); #52=IFCPCURVE(#40,#51);";
    step.product("guided-road", geometricSet + ",#48,#52");
    const auto model = step.load();
    EXPECT_FALSE(model.nativeCurveRanges.empty());
    const auto body = sectionedProductMesh(model, "guided-road");
    // Two planar strips of width 4 and sloped lengths (4,3) and (6,3).
    EXPECT_NEAR(checkSectionedMesh(body), 4 * (5 + std::sqrt(45.)), 1e-5);
    EXPECT_NEAR(maximumBodyHeight(body), 3., 1e-6);
    std::array<bool, 3> crest{};
    for (const auto i : body.indices) {
      const glm::dvec3 p = body.vertices.at(i).position;
      EXPECT_GE(p.x, -1e-6);
      EXPECT_LE(p.x, 10 + 1e-6);
      EXPECT_LE(std::abs(p.y), 2 + 1e-6);
      EXPECT_NEAR(p.z, std::min(.75 * p.x, .5 * (10 - p.x)), 1e-6);
      if (std::abs(p.x - 4) < 1e-6)
        for (size_t i = 0; i < crest.size(); ++i)
          crest[i] |= std::abs(p.y - (2. * i - 2.)) < 1e-6;
    }
    EXPECT_TRUE(
        std::ranges::all_of(crest, [](bool present) { return present; }));
    size_t crosswisePcurves = 0, longitudinalPcurves = 0;
    for (const auto &range : model.nativeCurveRanges) {
      const auto &first =
          model.vertices.at(model.indices.at(range.firstIndex)).position;
      const auto &last =
          model.vertices
              .at(model.indices.at(range.firstIndex + range.indexCount - 1))
              .position;
      if (glm::length(first - glm::vec3(4, -2, 3)) < 1e-6 &&
          glm::length(last - glm::vec3(4, 2, 3)) < 1e-6)
        ++crosswisePcurves;
      if (glm::length(first - glm::vec3(0, 0, 0)) > 1e-6 ||
          glm::length(last - glm::vec3(10, 0, 0)) > 1e-6)
        continue;
      // Both the authored Axis guide and the lifted chart reach this crest
      // with at least one line segment on each side of the offset knot.
      double distance = 0.;
      bool includesCrest = false;
      for (size_t i = range.firstIndex;
           i < size_t(range.firstIndex) + range.indexCount; i += 2) {
        const auto a = model.vertices.at(model.indices.at(i)).position;
        const auto b = model.vertices.at(model.indices.at(i + 1)).position;
        distance += glm::length(glm::dvec3(b - a));
        includesCrest |= glm::length(a - glm::vec3(4, 0, 3)) < 1e-6 ||
                         glm::length(b - glm::vec3(4, 0, 3)) < 1e-6;
      }
      EXPECT_TRUE(includesCrest);
      EXPECT_NEAR(distance, 5 + std::sqrt(45.), 1e-5);
      ++longitudinalPcurves;
    }
    EXPECT_EQ(crosswisePcurves, 1u);
    // The independent guide has different authored endpoints and direction.
    EXPECT_EQ(longitudinalPcurves, independent ? 1u : 2u);
  }
}

TEST(IfcCurveFamilies, SectionedGuidesBindByTagWithinProductRepresentations) {
  GuideSectionFixture step;
  const auto high =
      step.guide("crown", "#4", {{0, 0, 0}, {4, 0, 3}, {10, 0, 0}});
  const auto low =
      step.guide("crown", "#4", {{0, 0, 0}, {4, 0, 1}, {10, 0, 0}});
  const auto otherTag =
      step.guide("unused", "#4", {{0, 0, 0}, {4, 0, 8}, {10, 0, 0}});
  step.guide("crown", "#4", {{0, 0, 0}, {4, 0, 9}, {10, 0, 0}}); // orphan
  step.product("high", high + "," + high + "," + otherTag);
  step.product("low", low);
  step.product("unguided", otherTag);
  const auto model = step.load();
  for (const auto &diagnostic : model.importReport.diagnostics)
    ADD_FAILURE() << diagnostic.productGuid << " #" << diagnostic.entityId
                  << ": " << diagnostic.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_EQ(model.importReport.importedProductCount, 3u);
  EXPECT_FALSE(model.nativeCurveRanges.empty());
  for (const auto &[guid, height] :
       std::array<std::pair<const char *, double>, 3>{
           {{"high", 3.}, {"low", 1.}, {"unguided", 0.}}}) {
    SCOPED_TRACE(guid);
    const auto body = sectionedProductMesh(model, guid);
    EXPECT_FALSE(body.indices.empty());
    EXPECT_NEAR(maximumBodyHeight(body), height, 1e-6);
    if (!height)
      EXPECT_NEAR(checkSectionedMesh(body), 40., 1e-6);
  }
}

TEST(IfcCurveFamilies, SectionedGuidesRequireMatchingRepresentationFrame) {
  GuideSectionFixture step;
  const auto guide =
      step.guide("crown", "#4", {{0, 0, 0}, {4, 0, 3}, {10, 0, 0}});
  const auto mappedAxis = step.add(
      "IFCSHAPEREPRESENTATION($,'Axis','GeometricCurveSet',(" + guide + "))");
  const auto origin =
      step.add("IFCAXIS2PLACEMENT3D(" + step.point(0, 0, 0) + ",$,$)");
  const auto map =
      step.add("IFCREPRESENTATIONMAP(" + origin + "," + mappedAxis + ")");
  const auto target = step.add("IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$," +
                               step.point(0, 1, 0) + ",1.,$)");
  const auto mapped = step.add("IFCMAPPEDITEM(" + map + "," + target + ")");
  step.product("different-frame", mapped);
  step.product("same-frame", guide);
  const auto localBody =
      step.add("IFCSHAPEREPRESENTATION($,'Body','SectionedSurface',(#40))");
  const auto bodyMap =
      step.add("IFCREPRESENTATIONMAP(" + origin + "," + localBody + ")");
  const auto mappedBody =
      step.add("IFCMAPPEDITEM(" + bodyMap + "," + target + ")");
  step.product("both-mapped", mapped, mappedBody);
  const auto model = step.load();
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
  EXPECT_TRUE(sectionedProductMesh(model, "different-frame").indices.empty());
  EXPECT_NEAR(maximumBodyHeight(sectionedProductMesh(model, "same-frame")), 3.,
              1e-6);
  const auto translated = sectionedProductMesh(model, "both-mapped");
  EXPECT_NEAR(maximumBodyHeight(translated), 3., 1e-6);
  float minY = std::numeric_limits<float>::infinity(), maxY = -minY;
  for (const auto index : translated.indices) {
    const auto p = translated.vertices.at(index).position;
    minY = std::min(minY, p.y);
    maxY = std::max(maxY, p.y);
  }
  EXPECT_NEAR(minY, -1., 1e-6);
  EXPECT_NEAR(maxY, 3., 1e-6);
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u)
      << (model.importReport.diagnostics.empty()
              ? "Missing expected diagnostic"
              : model.importReport.diagnostics.back().reason);
  EXPECT_EQ(model.importReport.diagnostics.front().entityId, 40u);
  EXPECT_EQ(model.importReport.diagnostics.front().productGuid,
            "different-frame");
  EXPECT_NE(model.importReport.diagnostics.front().reason.find("frame"),
            std::string::npos);
}

TEST(IfcCurveFamilies, MalformedSectionedGuidesRetainAxisAndUnaffectedBody) {
  for (const std::string_view reason : {"disconnected", "duplicate", "crossing",
                                        "endpoint mismatch", "repeated tag"}) {
    SCOPED_TRACE(reason);
    GuideSectionFixture step;
    const auto crown =
        step.guide("crown", "#4", {{0, 0, 0}, {4, 0, 1}, {10, 0, 0}});
    auto guides = crown + ",";
    if (reason == "disconnected") {
      const auto a = step.point(2, 0, 0), b = step.point(8, 0, 0);
      const auto basis = step.add("IFCPOLYLINE((" + a + "," + b + "))");
      guides += step.guide("left", basis, {{0, -2, 0}, {6, -2, 0}});
    } else if (reason == "duplicate") {
      guides += step.guide("crown", "#4", {{0, 0, 0}, {4, 0, 2}, {10, 0, 0}});
    } else if (reason == "crossing") {
      guides += step.guide("left", "#4", {{0, -2, 0}, {4, .5, 0}, {10, -2, 0}});
    } else if (reason == "endpoint mismatch") {
      guides += step.guide("left", "#4", {{0, -2, -.25}, {10, -2, -.25}});
    } else {
      step.definitions +=
          "#31=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,0.,2.),(0.,0.,0.),"
          "('left','crown','crown','right'),#24);"
          "#41=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#31));";
      guides = crown;
    }
    step.product("rejected-body", guides,
                 reason == "repeated tag" ? "#41" : "#40");
    step.product("fallback");
    const auto model = step.load();
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
    EXPECT_FALSE(model.nativeCurveRanges.empty());
    EXPECT_TRUE(sectionedProductMesh(model, "rejected-body").indices.empty());
    EXPECT_NEAR(maximumBodyHeight(sectionedProductMesh(model, "fallback")), 0.,
                1e-6);
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId,
              reason == "repeated tag" ? 41u : 40u);
    EXPECT_EQ(model.importReport.diagnostics.front().productGuid,
              "rejected-body");
    EXPECT_FALSE(model.importReport.diagnostics.front().reason.empty());
  }
}

TEST(IfcCurveFamilies,
     SectionedHorizontalWidthsPreserveReversedProfileDirection) {
  auto definitions = guideSectionedFixture;
  replace(definitions, "((-2.,0.))", "((2.,0.))");
  // Project angle units are degrees. Widths remain positive even when
  // slopes direct both cross-section segments toward negative local X.
  replace(definitions, "(2.,2.),(0.,0.)", "(2.,2.),(180.,180.)");
  replace(definitions, "#30=", "#60=");
  replace(definitions, "(#30,#30)", "(#60,#60)");
  definitions += sweptDegreeUnits;
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
  EXPECT_NEAR(checkSectionedMesh(model, {0, 0, -1}), 40., 1e-6);
  std::set<float> widthAnchors;
  for (const auto &vertex : model.vertices) {
    EXPECT_NEAR(vertex.position.z, 0., 1e-6);
    widthAnchors.insert(vertex.position.y);
  }
  EXPECT_TRUE(widthAnchors.contains(-2));
  EXPECT_TRUE(widthAnchors.contains(0));
  EXPECT_TRUE(widthAnchors.contains(2));
}

// Flat profiles keep these topology and pcurve oracles independent of curved
// surface tessellation. The temporary D column splits from retained B at x=4.
const std::string transitionSections =
    sectionedStraightBase +
    "#25=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(4.),$,$,$,#4);"
    "#26=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(7.),$,$,$,#4);"
    "#27=IFCAXIS2PLACEMENTLINEAR(#25,$,$); "
    "#28=IFCAXIS2PLACEMENTLINEAR(#26,$,$);"
    "#29=IFCCARTESIANPOINT((2.,0.));"
    "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,2.),(0.,0.),('A','B','C'),#"
    "24);"
    "#31=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,0.,2.),(0.,0.,0.),('A','B','"
    "D','C'),#24);"
    "#32=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,1.,1.),(0.,0.,0.),('A','B','"
    "D','C'),#24);";

std::set<std::array<float, 3>> sectionedPoints(const Model &mesh) {
  std::set<std::array<float, 3>> points;
  for (const auto index : mesh.indices) {
    const auto p = mesh.vertices.at(index).position;
    points.insert({p.x, p.y, p.z});
  }
  return points;
}

std::vector<glm::dvec3> sectionedColumn(const std::string &definitions,
                                      double v, double last = 1.) {
  std::ostringstream uv;
  uv.precision(17);
  uv << "#41=IFCCARTESIANPOINT((0.," << v << "));"
     << "#42=IFCCARTESIANPOINT((" << last << ',' << v << "));"
     << "#43=IFCPOLYLINE((#41,#42)); #10=IFCPCURVE(#40,#43);";
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(definitions + uv.str()));
  complete(model);
  return vertices(model);
}

TEST(IfcCurveFamilies, SectionedTemporaryLaneStartsAndEndsAtAuthoredTips) {
  for (bool birth : {false, true}) {
    SCOPED_TRACE(birth);
    const auto definitions =
        transitionSections +
        (birth ? "#40=IFCSECTIONEDSURFACE(#4,(#22,#27,#23),(#30,#31,#32));"
               : "#40=IFCSECTIONEDSURFACE(#4,(#22,#27,#23),(#32,#31,#30));");
    const auto model =
        container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
    EXPECT_NEAR(checkSectionedMesh(model), 40., 1e-6);
    EXPECT_EQ(model.indices.size(), 27u);
    const auto points = sectionedPoints(model);
    EXPECT_EQ(points.size(), 10u);
    EXPECT_TRUE(points.contains({4, 0, 0}));
    EXPECT_TRUE(points.contains({birth ? 10.f : 0.f, 1, 0}));
    EXPECT_FALSE(points.contains({birth ? 0.f : 10.f, 1, 0}));
    const auto d = sectionedColumn(definitions, 2. / 3);
    ASSERT_FALSE(d.empty());
    for (const auto &p : d) {
      EXPECT_NEAR(
          p.y, birth ? std::max(0., (p.x - 4) / 6) : std::max(0., 1 - p.x / 4),
          1e-6);
      EXPECT_NEAR(p.z, 0., 1e-6);
    }
  }
}

TEST(IfcCurveFamilies, SectionedRepeatedRunsAnchorAnAdjacentTemporaryLane) {
  const auto definitions =
      transitionSections +
      "#33=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(1.,1.,2.),(0.,0.,0.),('A','B',"
      "'B','C'),#24);"
      "#34=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(1.,1.,0.,2.),(0.,0.,0.,0.),('"
      "A','B','B','D','C'),#24);"
      "#35=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(1.,1.,1.,1.),(0.,0.,0.,0.),('"
      "A','B','B','D','C'),#24);"
      "#40=IFCSECTIONEDSURFACE(#4,(#22,#27,#23),(#33,#34,#35));";
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
  EXPECT_NEAR(checkSectionedMesh(model), 40., 1e-6);
  EXPECT_EQ(model.indices.size(), 39u);
  EXPECT_EQ(sectionedPoints(model).size(), 13u);
  for (double v : {.25, .5, .75}) {
    const auto column = sectionedColumn(definitions, v);
    ASSERT_FALSE(column.empty());
    for (const auto &p : column) {
      const double expected = v == .25  ? -1.
                              : v == .5 ? 0.
                                        : std::max(0., (p.x - 4) / 6);
      EXPECT_NEAR(p.y, expected, 1e-6) << v;
    }
  }
}

TEST(IfcCurveFamilies,
     SectionedCoincidentTipSwapAndFullReversalKeepCorrespondence) {
  const auto definitions =
      transitionSections +
      "#33=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,0.,2.),(0.,0.,0.),('A','D',"
      "'B','C'),#24);"
      "#34=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(1.,1.,2.),"
      "(3.141592653589793,3.141592653589793,3.141592653589793),('C','D','B','A'"
      "),#29);"
      "#40=IFCSECTIONEDSURFACE(#4,(#22,#27,#23),(#32,#33,#34));";
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
  EXPECT_NEAR(checkSectionedMesh(model), 40., 1e-6);
  EXPECT_EQ(model.indices.size(), 30u);
  EXPECT_EQ(sectionedPoints(model).size(), 11u);
  const auto b = sectionedColumn(definitions, 1. / 3);
  const auto d = sectionedColumn(definitions, 2. / 3);
  ASSERT_FALSE(b.empty());
  ASSERT_FALSE(d.empty());
  for (const auto &p : b)
    EXPECT_NEAR(p.y, 0., 1e-6);
  for (const auto &p : d)
    EXPECT_NEAR(p.y, p.x < 4 ? (4 - p.x) / 4 : (p.x - 4) / 6, 1e-6);
}

TEST(IfcCurveFamilies, InvalidSectionedTagTransitionsRejectAtomically) {
  constexpr auto crossed = "#33=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,1.,1.)"
                           ",(0.,0.,0.),('A','D','B','C'),#24);";
  constexpr auto atC = "#34=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,2.,0.),(0."
                       ",0.,0.),('A','B','D','C'),#24);";
  for (const auto &[positions, profiles] :
       std::array<std::pair<const char *, const char *>, 3>{
           {{"(#22,#27,#23)", "(#30,#32,#32)"},
            {"(#22,#27,#23)", "(#32,#33,#32)"},
            {"(#22,#27,#28,#23)", "(#31,#30,#30,#34)"}}}) {
    SCOPED_TRACE(profiles);
    const auto definitions = transitionSections + crossed + atC +
                             "#40=IFCSECTIONEDSURFACE(#4," + positions + "," +
                             profiles + ");";
    const auto result =
        container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
    EXPECT_EQ(result.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(result.vertices.empty());
    EXPECT_TRUE(result.indices.empty());
    EXPECT_TRUE(result.meshRanges.empty());
    ASSERT_EQ(result.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(result.importReport.diagnostics.front().entityId, 40u);
    EXPECT_FALSE(result.importReport.diagnostics.front().reason.empty());
  }
}

TEST(IfcCurveFamilies, SectionedTemporaryLaneGuideFollowsItsAuthoredSpan) {
  for (const bool birth : {false, true}) {
    SCOPED_TRACE(birth);
    GuideSectionFixture step;
    step.definitions =
        transitionSections +
        (birth ? "#40=IFCSECTIONEDSURFACE(#4,(#22,#27,#23),(#30,#31,#32));"
               : "#40=IFCSECTIONEDSURFACE(#4,(#22,#27,#23),(#32,#31,#30));") +
        "#41=IFCCARTESIANPOINT((0.1,0.66666666666666667));"
        "#42=IFCCARTESIANPOINT((0.9,0.66666666666666667));"
        "#43=IFCPOLYLINE((#41,#42)); #45=IFCPCURVE(#40,#43);";
    const auto retained = step.guide(
        "B", "#4", {{0, 0, 0}, {2, -.25, 0}, {4, 0, 0}, {7, -.25, 0},
                    {10, 0, 0}});
    std::string temporary;
    if (birth) {
      temporary = step.guide("D", "#4", {{4, 0, 0}, {7, .75, 0}, {10, 1, 0}});
    } else {
      const auto start = step.point(0, 1, 0);
      const auto middle = step.point(2, .75, 0);
      const auto tip = step.point(4, 0, 0);
      const auto rail = step.add("IFCPOLYLINE((" + start + "," + middle + "," +
                                 tip + "))");
      temporary = step.guide("D", rail, {{0, 0, 0}});
    }
    step.product("guided-branch", retained + "," + temporary + ",#45");
    const auto model = step.load();
    const auto body = sectionedProductMesh(model, "guided-branch");
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(checkSectionedMesh(body), 40., 1e-6);
    const auto points = sectionedPoints(body);
    const float activeStation = birth ? 7.f : 2.f;
    const float collapsedStation = birth ? 2.f : 7.f;
    EXPECT_TRUE(points.contains({activeStation, .75f, 0}));
    EXPECT_TRUE(points.contains({collapsedStation, -.25f, 0}));
    EXPECT_FALSE(points.contains({collapsedStation, 0, 0}));
    EXPECT_TRUE(points.contains({4, 0, 0}));

    size_t branchCurves = 0;
    for (const auto &range : model.nativeCurveRanges) {
      const auto pointAt = [&](size_t i) {
        return model.vertices.at(model.indices.at(range.firstIndex + i))
            .position;
      };
      if (pointAt(0).x != 1.f || pointAt(range.indexCount - 1).x != 9.f)
        continue;
      ++branchCurves;
      std::set<std::array<float, 3>> samples;
      for (size_t i = 0; i < range.indexCount; ++i) {
        const auto p = pointAt(i);
        samples.insert({p.x, p.y, p.z});
      }
      EXPECT_TRUE(samples.contains({activeStation, .75f, 0}));
      EXPECT_TRUE(samples.contains({collapsedStation, -.25f, 0}));
      EXPECT_TRUE(samples.contains({4, 0, 0}));
    }
    EXPECT_EQ(branchCurves, 1u);
  }
}

TEST(IfcCurveFamilies, SectionedTemporaryLaneGuideRequiresOneAuthoredRun) {
  for (const bool disjoint : {false, true}) {
    SCOPED_TRACE(disjoint);
    GuideSectionFixture step;
    step.definitions =
        transitionSections +
        "#35=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(5.),$,$,$,#4);"
        "#36=IFCAXIS2PLACEMENTLINEAR(#35,$,$);" +
        std::string(disjoint ? "#40=IFCSECTIONEDSURFACE(#4,"
                               "(#22,#27,#36,#28,#23),(#32,#31,#30,#31,#32));"
                             : "#40=IFCSECTIONEDSURFACE(#4,"
                               "(#22,#27,#23),(#30,#31,#30));");
    const auto guide = step.guide("D", "#4", {{0, 0, 0}});
    step.product("ambiguous-guide", guide);
    const auto model = step.load();
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
    EXPECT_TRUE(sectionedProductMesh(model, "ambiguous-guide").indices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    const auto &diagnostic = model.importReport.diagnostics.front();
    EXPECT_EQ(diagnostic.entityId, 40u);
    const auto reason = disjoint
                            ? "intermittent tag 'D' requires one contiguous run"
                            : "Ambiguous section tag order for 'B' and 'D'";
    EXPECT_TRUE(diagnostic.reason.contains(reason)) << diagnostic.reason;
  }
}

TEST(IfcCurveFamilies, SectionedGuideMustAgreeWithItsAuthoredBranchTip) {
  for (bool temporary : {false, true}) {
    SCOPED_TRACE(temporary);
    GuideSectionFixture step;
    step.definitions =
        transitionSections +
        "#40=IFCSECTIONEDSURFACE(#4,(#22,#27,#23),(#30,#31,#32));";
    const auto guide =
        temporary ? step.guide("D", "#4", {{4, .25, 0}, {10, 1, 0}})
                  : step.guide("B", "#4",
                               {{0, 0, 0},
                                {2, -.25, 0},
                                {4, 0, 0},
                                {7, -.5, 0},
                                {10, 0, 0}});
    auto axis = guide;
    if (!temporary) {
      // Lift both longitudinal columns between x=2, where D is collapsed
      // onto B, and x=7, where the columns must again be distinct.
      step.definitions += "#41=IFCCARTESIANPOINT((0.2,0.33333333333333333));"
                          "#42=IFCCARTESIANPOINT((0.2,0.66666666666666667));"
                          "#46=IFCCARTESIANPOINT((0.7,0.33333333333333333));"
                          "#47=IFCCARTESIANPOINT((0.7,0.66666666666666667));"
                          "#43=IFCPOLYLINE((#41,#46)); #45=IFCPCURVE(#40,#43);"
                          "#48=IFCPOLYLINE((#42,#47)); #49=IFCPCURVE(#40,#48);";
      axis += ",#45,#49";
    }
    step.product("branching", axis);
    const auto result = step.load();
    const auto body = sectionedProductMesh(result, "branching");
    EXPECT_FALSE(result.nativeCurveRanges.empty());
    if (temporary) {
      EXPECT_EQ(result.importReport.completeness, ImportCompleteness::Partial);
      EXPECT_TRUE(body.indices.empty());
      ASSERT_EQ(result.importReport.diagnostics.size(), 1u);
      EXPECT_EQ(result.importReport.diagnostics.front().entityId, 40u);
      EXPECT_TRUE(result.importReport.diagnostics.front().reason.contains(
          "for tag 'D' disagrees with its authored cross-section point"))
          << result.importReport.diagnostics.front().reason;
    } else {
      EXPECT_NEAR(checkSectionedMesh(body), 40., 1e-6);
      const auto points = sectionedPoints(body);
      EXPECT_TRUE(points.contains({2, -.25f, 0}));
      EXPECT_FALSE(points.contains({2, 0, 0}));
      EXPECT_TRUE(points.contains({7, -.5f, 0}));
      EXPECT_TRUE(points.contains({7, .5f, 0}));
      std::set<float> liftedEndpoints;
      for (const auto &range : result.nativeCurveRanges) {
        const auto first =
            result.vertices.at(result.indices.at(range.firstIndex)).position;
        const auto last =
            result.vertices
                .at(result.indices.at(range.firstIndex + range.indexCount - 1))
                .position;
        if (first.x != 2.f || last.x != 7.f)
          continue;
        EXPECT_EQ(first, glm::vec3(2, -.25f, 0));
        EXPECT_EQ(last.z, 0.f);
        liftedEndpoints.insert(last.y);
        const bool stableColumn = last.y < 0.f;
        const std::array stations{3., 4., 4.01, 5., 6.};
        std::array<bool, stations.size()> observed{};
        for (size_t i = range.firstIndex;
             i < size_t(range.firstIndex) + range.indexCount; i += 2) {
          const auto a = result.vertices.at(result.indices.at(i)).position;
          const auto b = result.vertices.at(result.indices.at(i + 1)).position;
          for (size_t j = 0; j < stations.size(); ++j) {
            const double x = stations[j];
            if (x < a.x || x > b.x || a.x == b.x)
              continue;
            observed[j] = true;
            const auto y =
                std::lerp(double(a.y), double(b.y), (x - a.x) / (b.x - a.x));
            const auto expected = x <= 4         ? (x - 4) / 8
                                  : stableColumn ? -(x - 4) / 6
                                                 : (x - 4) / 6;
            EXPECT_NEAR(y, expected, 1e-6) << x << " " << stableColumn;
          }
        }
        EXPECT_TRUE(std::ranges::all_of(observed,
                                        [](bool present) { return present; }));
      }
      EXPECT_EQ(liftedEndpoints, (std::set<float>{-.5f, .5f}));
    }
  }
}

TEST(IfcCurveFamilies, SectionedTagsSplitAndMergeWithoutLosingBreaklines) {
  for (bool merge : {false, true}) {
    auto definitions = branchingSectionedFixture;
    if (merge) {
      replace(definitions, "(#30,#31)", "(#31,#30)");
    }
    const auto source = fixture(definitions, "#40");
    const auto model = container::geometry::ifc::LoadFromStep(source);
    EXPECT_NEAR(checkSectionedMesh(model), 40., 1e-6);
    EXPECT_EQ(model.indices.size(), 15u);
    for (const auto &[v, sign] : std::array<std::pair<double, double>, 2>{
             {{1. / 3, -1}, {2. / 3, 1}}}) {
      std::ostringstream stream;
      stream.precision(17);
      stream << definitions << "#41=IFCCARTESIANPOINT((0.," << v << "));"
             << "#42=IFCCARTESIANPOINT((1.," << v << "));"
             << "#43=IFCPOLYLINE((#41,#42)); #10=IFCPCURVE(#40,#43);";
      const auto line =
          container::geometry::ifc::LoadFromStep(fixture(stream.str()));
      complete(line);
      for (auto p : vertices(line))
        EXPECT_NEAR(p.y, sign * (merge ? 1 - p.x / 10 : p.x / 10), 1e-6);
    }
    exportSectionedFixture(merge ? "tag-merge" : "tag-split", source);
  }
}

TEST(IfcCurveFamilies,
     MultipleSectionedTagTransitionsPreserveStationsAndUnits) {
  for (bool millimetres : {false, true}) {
    auto definitions = branchingSectionedFixture;
    definitions +=
        "#50=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(5.),$,$,$,#4);"
        "#51=IFCAXIS2PLACEMENTLINEAR(#50,$,$);";
    replace(definitions, "(#22,#23),(#30,#31)", "(#22,#51,#23),(#30,#31,#30)");
    if (millimetres) {
      for (const auto &[from, to] :
           std::vector<std::pair<std::string, std::string>>{
               {"((10.,0.,0.))", "((10000.,0.,0.))"},
               {"IFCLENGTHMEASURE(10.)", "IFCLENGTHMEASURE(10000.)"},
               {"IFCLENGTHMEASURE(5.)", "IFCLENGTHMEASURE(5000.)"},
               {"((-2.,0.))", "((-2000.,0.))"},
               {"(2.,2.),(0.,0.)", "(2000.,2000.),(0.,0.)"},
               {"(1.,2.,1.),(0.,0.,0.)", "(1000.,2000.,1000.),(0.,0.,0.)"}})
        replace(definitions, from, to);
    }
    auto source = fixture(definitions, "#40");
    if (millimetres)
      replace(source, ".LENGTHUNIT.,$,.METRE.", ".LENGTHUNIT.,.MILLI.,.METRE.");
    const auto model = container::geometry::ifc::LoadFromStep(source);
    const double scale = millimetres ? 1000 : 1;
    EXPECT_NEAR(checkSectionedMesh(model) / (scale * scale), 40., 1e-6);
    EXPECT_EQ(model.indices.size(), 30u);
    bool foundFirst = false, foundSecond = false;
    for (const auto &vertex : model.vertices) {
      const auto p = vertex.position / static_cast<float>(scale);
      foundFirst |= p == glm::vec3(5, -1, 0);
      foundSecond |= p == glm::vec3(5, 1, 0);
    }
    EXPECT_TRUE(foundFirst);
    EXPECT_TRUE(foundSecond);
  }
}

TEST(IfcCurveFamilies, SectionedMultiwayBranchesRetainEverySourceCorner) {
  auto definitions = branchingSectionedFixture;
  replace(definitions, "(2.,2.),(0.,0.),('left','branch','right')",
          "(1.,1.,1.,1.),(0.,0.,0.,0.),('left','branch','branch','branch','"
          "right')");
  replace(definitions,
          "(1.,2.,1.),(0.,0.,0.),('left','branch','branch','right')",
          "(0.5,1.,1.,1.,0.5),(0.,0.,0.,0.,0.),('left','branch','branch','"
          "branch','branch','right')");
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
  EXPECT_NEAR(checkSectionedMesh(model), 40., 1e-6);
  for (const auto &[station, positions] :
       std::array<std::pair<float, std::vector<float>>, 2>{
           {{0.f, {-2, -1, 0, 1, 2}}, {10.f, {-2, -1.5f, -.5f, .5f, 1.5f, 2}}}})
    for (float y : positions) {
      bool found = false;
      for (const auto &vertex : model.vertices)
        found |= vertex.position == glm::vec3(station, y, 0);
      EXPECT_TRUE(found) << station << ", " << y;
    }
}

TEST(IfcCurveFamilies, ZeroWidthSectionedBranchesRetainSharedTips) {
  for (bool opens : {false, true}) {
    for (bool tagged : {false, true}) {
      auto definitions = branchingSectionedFixture;
      replace(definitions, "(2.,2.),(0.,0.),('left','branch','right')",
              "(2.,0.,2.),(0.,0.,0.),('left','branch','branch','right')");
      if (!opens)
        replace(definitions, "(1.,2.,1.)", "(2.,0.,2.)");
      if (!tagged) {
        replace(definitions, "('left','branch','branch','right')", "$");
        replace(definitions, "('left','branch','branch','right')", "$");
      }
      const auto model =
          container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
      EXPECT_NEAR(checkSectionedMesh(model), 40., 1e-6);
      EXPECT_EQ(model.indices.size(), opens ? 15u : 12u);
    }
  }
  for (const auto &widths : {"(0.,0.)", "(-1.,2.)"}) {
    auto definitions = branchingSectionedFixture;
    replace(definitions, "(2.,2.)", widths);
    const auto model =
        container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 40u);
  }
}

TEST(IfcCurveFamilies, SlopedSectionedBranchesRetainCrownAndChordAccuracy) {
  for (bool merge : {false, true}) {
    auto definitions = branchingSectionedFixture;
    replace(definitions, "(2.,2.),(0.,0.)",
            "(2.,2.),(0.09966865249116204,-0.09966865249116204)");
    replace(definitions, "(1.,2.,1.),(0.,0.,0.)",
            "(1.,2.,1.),(0.19739555984988078,0.,-0.19739555984988078)");
    if (merge)
      replace(definitions, "(#30,#31)", "(#31,#30)");
    const auto source = fixture(definitions, "#40");
    const auto model = container::geometry::ifc::LoadFromStep(source);
    checkSectionedMesh(model);
    const auto height = [merge](glm::dvec3 p) {
      const double width = merge ? 1 - p.x / 10 : p.x / 10;
      return std::abs(p.y) <= width ? .2
                                    : .2 * (2 - std::abs(p.y)) / (2 - width);
    };
    double projectedArea = 0;
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      std::array<glm::dvec3, 3> p;
      for (unsigned j = 0; j < 3; ++j) {
        p[j] = model.vertices.at(model.indices.at(i + j)).position;
        EXPECT_NEAR(p[j].z, height(p[j]), 1e-6);
      }
      projectedArea += glm::cross(p[1] - p[0], p[2] - p[0]).z / 2;
      for (auto q : {(p[0] + p[1] + p[2]) / 3., (p[0] + p[1]) / 2.,
                     (p[1] + p[2]) / 2., (p[2] + p[0]) / 2.})
        EXPECT_NEAR(q.z, height(q), .001);
    }
    EXPECT_NEAR(projectedArea, 40., 1e-5);
    exportSectionedFixture(merge ? "tag-merge-crown" : "tag-split-crown",
                           source);
  }
}

TEST(IfcCurveFamilies, CurvedSectionedBranchesKeepAnnularAreaAndSharedTips) {
  auto definitions = branchingSectionedFixture;
  replace(definitions, "#4=IFCPOLYLINE((#2,#3));",
          "#5=IFCAXIS2PLACEMENT3D(#2,$,$); #4=IFCCIRCLE(#5,5.);");
  replace(definitions, "IFCLENGTHMEASURE(10.)",
          "IFCPARAMETERVALUE(1.5707963267948966)");
  const auto source = fixture(definitions, "#40");
  const auto model = container::geometry::ifc::LoadFromStep(source);
  EXPECT_NEAR(checkSectionedMesh(model), 10 * pi, .012);
  for (const auto &vertex : model.vertices) {
    EXPECT_GE(glm::length(glm::vec2(vertex.position)), 3 - 1e-6);
    EXPECT_LE(glm::length(glm::vec2(vertex.position)), 7 + 1e-6);
  }
  exportSectionedFixture("curved-branch", source);
}

const std::string miterSectionedFixture =
    "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((10.,0.,0.));"
    "#5=IFCCARTESIANPOINT((10.,10.,0.)); #4=IFCPOLYLINE((#2,#3,#5));"
    "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(0.),$,$,$,#4);"
    "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCLENGTHMEASURE(20.),$,$,$,#4);"
    "#22=IFCAXIS2PLACEMENTLINEAR(#20,$,$); "
    "#23=IFCAXIS2PLACEMENTLINEAR(#21,$,$);"
    "#24=IFCCARTESIANPOINT((-2.,0.));"
    "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.,2.),(0.,0.),('left','crown','"
    "right'),#24);"
    "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#30));";

std::string independentMiterGuide(GuideSectionFixture &step,
                                  std::initializer_list<glm::dvec2> points) {
  std::string refs;
  for (const auto p : points) {
    if (!refs.empty())
      refs += ',';
    refs += step.point(p.x, p.y, 0);
  }
  const auto basis = step.add("IFCPOLYLINE((" + refs + "))");
  return step.guide("left", basis, {{0, 0, 0}});
}

TEST(IfcCurveFamilies, SectionedMiterGuidesKeepBulgedRailAndSharedRow) {
  GuideSectionFixture step;
  step.definitions = miterSectionedFixture;
  const auto guide =
      independentMiterGuide(step, {{0, -2}, {6, -3}, {12, -2}, {12, 10}});
  constexpr double knot = 6. / 13.;
  const auto uv = [&](double u, double v) {
    std::ostringstream point;
    point.precision(17);
    point << "IFCCARTESIANPOINT((" << u << ',' << v << "))";
    return step.add(point.str());
  };
  const auto pcurve = [&](glm::dvec2 a, glm::dvec2 b) {
    const auto line =
        step.add("IFCPOLYLINE((" + uv(a.x, a.y) + "," + uv(b.x, b.y) + "))");
    return step.add("IFCPCURVE(#40," + line + ")");
  };
  const auto rail = pcurve({0, 0}, {2, 0});
  const auto row = pcurve({knot, 0}, {knot, 1});
  const auto axis = step.add("IFCGEOMETRICCURVESET((" + guide + "," + rail +
                             "," + row + "))");
  step.product("guided-miter", axis);

  const auto frame =
      step.add("IFCAXIS2PLACEMENT3D(" + step.point(0, 0, 0) + ",$,$)");
  const auto target = step.add("IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$," +
                               step.point(30, 5, 1) + ",2.,$)");
  const auto mapped = [&](const std::string &representation) {
    const auto map =
        step.add("IFCREPRESENTATIONMAP(" + frame + "," + representation + ")");
    return step.add("IFCMAPPEDITEM(" + map + "," + target + ")");
  };
  const auto axisRep = step.add(
      "IFCSHAPEREPRESENTATION($,'Axis','GeometricCurveSet',(" + axis + "))");
  const auto bodyRep =
      step.add("IFCSHAPEREPRESENTATION($,'Body','SectionedSurface',(#40))");
  step.product("mapped-guided-miter", mapped(axisRep), mapped(bodyRep));

  const auto model = step.load();
  const std::array<glm::dvec3, 6> required{{{6, -3, 0},
                                            {60. / 13., 0, 0},
                                            {48. / 13., 2, 0},
                                            {12, -2, 0},
                                            {10, 0, 0},
                                            {8, 2, 0}}};
  for (const bool transformed : {false, true}) {
    SCOPED_TRACE(transformed);
    const auto body = sectionedProductMesh(
        model, transformed ? "mapped-guided-miter" : "guided-miter");
    const double scale = transformed ? 2. : 1.;
    const glm::dvec3 translation =
        transformed ? glm::dvec3(30, 5, 1) : glm::dvec3(0);
    // The bulge adds a triangular wedge of base 12 m and height 1 m.
    EXPECT_NEAR(checkSectionedMesh(body), 86 * scale * scale, 1e-4);
    for (const auto &point : required) {
      const auto expected = translation + scale * point;
      EXPECT_TRUE(std::ranges::any_of(
          body.indices,
          [&](uint32_t index) {
            return glm::length(glm::dvec3(body.vertices.at(index).position) -
                               expected) < 1e-5;
          }))
          << expected.x << ',' << expected.y << ',' << expected.z;
    }
  }

  size_t railCurves = 0, rowCurves = 0;
  for (const auto &range : model.nativeCurveRanges) {
    const auto p = [&](size_t offset) {
      return glm::dvec3(
          model.vertices.at(model.indices.at(range.firstIndex + offset))
              .position);
    };
    const auto a = p(0), b = p(range.indexCount - 1);
    if (glm::length(a - glm::dvec3(0, -2, 0)) < 1e-6 &&
        glm::length(b - glm::dvec3(12, 10, 0)) < 1e-6) {
      ++railCurves;
      double distance = 0.;
      bool hasKnot = false, hasMiter = false;
      for (size_t j = 0; j < range.indexCount; j += 2) {
        distance += glm::length(p(j + 1) - p(j));
        for (const auto point : {p(j), p(j + 1)}) {
          hasKnot |= point == glm::dvec3(6, -3, 0);
          hasMiter |= point == glm::dvec3(12, -2, 0);
        }
      }
      EXPECT_NEAR(distance, 12. + 2. * std::sqrt(37.), 1e-5);
      EXPECT_TRUE(hasKnot && hasMiter);
    }
    if (glm::length(a - required[0]) < 1e-5 &&
        glm::length(b - required[2]) < 1e-5)
      ++rowCurves;
  }
  EXPECT_GE(railCurves, 2u) << "Expected independent guide and lifted rail";
  EXPECT_GE(rowCurves, 1u) << "Expected cross section at native U=6/13";
}

TEST(IfcCurveFamilies, SectionedMiterGuidesRejectAmbiguousCornerPlaneCrossings) {
  GuideSectionFixture step;
  step.definitions = miterSectionedFixture;
  // Signed distance to the miter plane X+Y=10 changes sign three times.
  const auto guide = independentMiterGuide(
      step, {{0, -2}, {10, -2}, {12, -1}, {10, -.5}, {12, 1}, {12, 10}});
  const auto axis = step.add("IFCGEOMETRICCURVESET((" + guide + "))");
  step.product("invalid-miter-guide", axis);
  step.product("unguided-miter");
  const auto model = step.load();
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
  EXPECT_FALSE(model.nativeCurveRanges.empty());
  EXPECT_TRUE(
      sectionedProductMesh(model, "invalid-miter-guide").indices.empty());
  EXPECT_FALSE(sectionedProductMesh(model, "unguided-miter").indices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &diagnostic = model.importReport.diagnostics.front();
  EXPECT_EQ(diagnostic.entityId, 40u);
  EXPECT_EQ(diagnostic.productGuid, "invalid-miter-guide");
  EXPECT_NE(diagnostic.reason.find(guide), std::string::npos)
      << diagnostic.reason;
  // Before the bisecting plane acquires its three crossings, the sweeping
  // plane first touches the guide's intervening vertex.
  EXPECT_NE(diagnostic.reason.find("touches a section plane without crossing"),
            std::string::npos)
      << diagnostic.reason;
}

TEST(IfcCurveFamilies, SectionedMiterGuidesKeepSharedBasisVerticalCrown) {
  GuideSectionFixture step;
  step.definitions = miterSectionedFixture;
  const auto guide =
      step.guide("crown", "#4", {{0, 0, 0}, {4, 0, 2}, {10, 0, 0}, {20, 0, 0}});
  step.definitions +=
      "#45=IFCCARTESIANPOINT((0.,0.5)); #46=IFCCARTESIANPOINT((2.,0.5));"
      "#47=IFCPOLYLINE((#45,#46)); #48=IFCPCURVE(#40,#47);";
  const auto axis = step.add("IFCGEOMETRICCURVESET((" + guide + ",#48))");
  step.product("vertical-miter-guide", axis);
  const auto model = step.load();
  const auto body = sectionedProductMesh(model, "vertical-miter-guide");
  EXPECT_GT(checkSectionedMesh(body), 80.);
  EXPECT_NEAR(maximumBodyHeight(body), 2., 1e-6);
  // The guide raises the crown at U=.4. The unbound outer columns retain
  // their original positions in the same ruled row and at the miter.
  const std::array<glm::dvec3, 6> expected{{{4.8, -2, 0},
                                            {4, 0, 2},
                                            {3.2, 2, 0},
                                            {12, -2, 0},
                                            {10, 0, 0},
                                            {8, 2, 0}}};
  for (const auto point : expected) {
    EXPECT_TRUE(std::ranges::any_of(
        body.indices,
        [&](uint32_t index) {
          return glm::length(glm::dvec3(body.vertices.at(index).position) -
                             point) < 1e-6;
        }))
        << point.x << ',' << point.y << ',' << point.z;
  }

  size_t crownCurves = 0;
  for (const auto &range : model.nativeCurveRanges) {
    std::vector<glm::dvec3> points;
    for (size_t j = 0; j < range.indexCount; ++j)
      points.emplace_back(
          model.vertices.at(model.indices.at(range.firstIndex + j)).position);
    if (points.empty() || points.front() != glm::dvec3(0) ||
        points.back() != glm::dvec3(10, 10, 0))
      continue;
    ++crownCurves;
    ASSERT_EQ(points.size() % 2, 0u);
    EXPECT_EQ(std::ranges::count(points, glm::dvec3(4, 0, 2)), 2);
    EXPECT_EQ(std::ranges::count(points, glm::dvec3(10, 0, 0)), 2);
    EXPECT_NEAR(length(points), std::sqrt(20.) + std::sqrt(40.) + 10., 1e-5);
  }
  EXPECT_EQ(crownCurves, 2u) << "Expected shared guide and lifted crown pcurve";
}

TEST(IfcCurveFamilies, SectionedMiterGuidesRejectSharedBasisLateralOffset) {
  GuideSectionFixture step;
  step.definitions = miterSectionedFixture;
  // The lateral offset vanishes at both endpoint anchors and at the corner.
  // Its standalone Axis curve stays continuous, but leaves the ruled plane.
  const auto guide = step.guide(
      "crown", "#4", {{0, 0, 0}, {4, -1, 0}, {10, 0, 0}, {20, 0, 0}});
  const auto axis = step.add("IFCGEOMETRICCURVESET((" + guide + "))");
  step.product("lateral-miter-guide", axis);
  const auto model = step.load();
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
  EXPECT_FALSE(model.nativeCurveRanges.empty());
  EXPECT_TRUE(
      sectionedProductMesh(model, "lateral-miter-guide").indices.empty());
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &diagnostic = model.importReport.diagnostics.front();
  EXPECT_EQ(diagnostic.entityId, 40u);
  EXPECT_NE(diagnostic.reason.find(guide), std::string::npos)
      << diagnostic.reason;
  EXPECT_NE(diagnostic.reason.find("can only offset vertically"),
            std::string::npos)
      << diagnostic.reason;
}

TEST(IfcCurveFamilies, SectionedSharpJoinsUseSharedHalfAngleMiters) {
  for (bool reversed : {false, true}) {
    for (bool branched : {false, true}) {
      auto definitions = miterSectionedFixture;
      if (reversed)
        replace(definitions, "IFCPOLYLINE((#2,#3,#5))",
                "IFCPOLYLINE((#5,#3,#2))");
      if (branched) {
        definitions +=
            "#31=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(1.,2.,1.),(0.,0.,0.),"
            "('left','crown','crown','right'),#24);";
        replace(definitions, "(#30,#30)", "(#30,#31)");
      }
      const auto source = fixture(definitions, "#40");
      const auto model = container::geometry::ifc::LoadFromStep(source);
      EXPECT_NEAR(checkSectionedMesh(model), 80., 1e-6);
      bool inner = false, outer = false;
      for (const auto &vertex : model.vertices) {
        inner |= vertex.position == glm::vec3(8, 2, 0);
        outer |= vertex.position == glm::vec3(12, -2, 0);
      }
      EXPECT_TRUE(inner);
      EXPECT_TRUE(outer);
      for (double v : {0., 1.}) {
        auto lineDefs = definitions + "#41=IFCCARTESIANPOINT((0.," +
                        std::to_string(v) +
                        "));"
                        "#42=IFCCARTESIANPOINT((2.," +
                        std::to_string(v) +
                        "));"
                        "#43=IFCPOLYLINE((#41,#42)); #10=IFCPCURVE(#40,#43);";
        const auto line =
            container::geometry::ifc::LoadFromStep(fixture(lineDefs));
        complete(line);
        EXPECT_NEAR(length(vertices(line)), (v == 0) != reversed ? 24 : 16,
                    1e-5);
      }
      if (!reversed)
        exportSectionedFixture(branched ? "miter-branch" : "miter", source);
    }
  }
}

TEST(IfcCurveFamilies, SectionedMitersPreserveRotatedCrossPlanes) {
  auto definitions = miterSectionedFixture;
  replace(definitions, "((10.,10.,0.))", "((10.,0.,-10.))");
  definitions += "#6=IFCDIRECTION((0.,1.,0.));";
  replace(definitions, "(#20,$,$)", "(#20,#6,$)");
  replace(definitions, "(#21,$,$)", "(#21,#6,$)");
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
  EXPECT_NEAR(checkSectionedMesh(model, {0, 1, 0}), 80., 1e-6);
  bool inner = false, outer = false;
  for (const auto &vertex : model.vertices) {
    inner |= vertex.position == glm::vec3(8, 0, -2);
    outer |= vertex.position == glm::vec3(12, 0, 2);
  }
  EXPECT_TRUE(inner);
  EXPECT_TRUE(outer);
}

const std::string nonplanarSharpSections =
    "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((8.,0.,0.));"
    "#5=IFCCARTESIANPOINT((8.,6.,0.)); #6=IFCCARTESIANPOINT((8.,12.,8.));"
    "#4=IFCPOLYLINE((#2,#3,#5,#6));"
    "#8=IFCDIRECTION((0.,0.,1.)); #9=IFCDIRECTION((0.,-0.8,0.6));"
    "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(0.),$,$,$,#4);"
    "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(3.),$,$,$,#4);"
    "#22=IFCAXIS2PLACEMENTLINEAR(#20,#8,$);"
    "#23=IFCAXIS2PLACEMENTLINEAR(#21,#9,$);"
    "#24=IFCCARTESIANPOINT((-1.,0.));"
    "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.),(0.),$, #24);"
    "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#30));";

TEST(IfcCurveFamilies, SectionedNonplanarSharpJoinsShareExactBisectorRows) {
  for (bool raised : {false, true}) {
    SCOPED_TRACE(raised);
    auto definitions = nonplanarSharpSections;
    if (raised)
      replace(definitions, "((-1.,0.))", "((-1.,1.))");
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(definitions, "#40", "'Surface','SectionedSurface'"));
    const double h = raised ? 1. : 0.;
    EXPECT_NEAR(checkSectionedMesh(model), 48. - 2 * h, 1e-5);
    const auto rowPoints = sectionedPoints(model);
    const auto contains = [&](glm::dvec3 expected) {
      return std::ranges::any_of(rowPoints, [&](const auto &p) {
        return glm::length(glm::dvec3(p[0], p[1], p[2]) - expected) < 1e-6;
      });
    };
    for (float r : {-1.f, 1.f}) {
      // At the first join the two generators agree at x=8-r, y=r.
      // At the second, projection onto normal (0,1.6,.8) shifts the
      // incoming generator backward by h/2 along the +Y tangent.
      EXPECT_TRUE(contains({8 - r, r, h}));
      EXPECT_TRUE(contains({8 - r, 6 - h / 2, h}));
      const auto line = sectionedColumn(definitions, (r + 1.) / 2, 3.);
      ASSERT_FALSE(line.empty());
      EXPECT_LT(glm::length(line.front() - glm::dvec3(0, r, h)), 1e-6);
      EXPECT_LT(
          glm::length(line.back() - glm::dvec3(8 - r, 12 - .8 * h, 8 + .6 * h)),
          1e-6);
      for (const auto expected :
           {glm::dvec3(8 - r, r, h), glm::dvec3(8 - r, 6 - h / 2, h)})
        EXPECT_TRUE(std::ranges::any_of(line, [&](const auto &p) {
          return glm::length(p - expected) < 1e-6;
        }));
    }
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      const glm::dvec3 a(model.vertices.at(model.indices.at(i)).position),
          b(model.vertices.at(model.indices.at(i + 1)).position),
          c(model.vertices.at(model.indices.at(i + 2)).position);
      const auto expected = std::max({a.z, b.z, c.z}) > h + 1e-6
                                ? glm::dvec3(0, -.8, .6)
                                : glm::dvec3(0, 0, 1);
      EXPECT_GT(glm::dot(glm::normalize(glm::cross(b - a, c - a)), expected),
                .999999);
    }
  }
}

const std::string curvedSharpSections =
    "#2=IFCCARTESIANPOINT((0.,12.,0.)); #3=IFCDIRECTION((0.,0.,1.));"
    "#5=IFCDIRECTION((0.,-1.,0.)); #6=IFCAXIS2PLACEMENT3D(#2,#3,#5);"
    "#7=IFCCIRCLE(#6,12.);"
    "#8=IFCTRIMMEDCURVE(#7,(IFCPARAMETERVALUE(0.)),"
    "(IFCPARAMETERVALUE(1.5707963267948966)),.T.,.PARAMETER.);"
    "#9=IFCCARTESIANPOINT((12.,12.,0.)); #19=IFCCARTESIANPOINT((24.,12.,0.));"
    "#11=IFCPOLYLINE((#9,#19));"
    "#12=IFCREPARAMETRISEDCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#8,1.);"
    "#13=IFCCOMPOSITECURVESEGMENT(.DISCONTINUOUS.,.T.,#11);"
    "#4=IFCCOMPOSITECURVE((#12,#13),.F.);"
    "#20=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(0.),$,$,$,#4);"
    "#21=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(2.),$,$,$,#4);"
    "#22=IFCAXIS2PLACEMENTLINEAR(#20,#3,$); "
    "#23=IFCAXIS2PLACEMENTLINEAR(#21,#3,$);"
    "#24=IFCCARTESIANPOINT((-1.,0.));"
    "#30=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(2.),(0.),$,#24);"
    "#40=IFCSECTIONEDSURFACE(#4,(#22,#23),(#30,#30));";

TEST(IfcCurveFamilies, SectionedSharpCornerSurvivesNearAuthoredStation) {
  auto definitions = nonplanarSharpSections;
  // The last two tangents lie in the YZ plane, so the flat lateral profile
  // is parallel to their common X binormal and needs no longitudinal shift.
  // Put their join at the origin in Y, where 0.4 micrometres still resolves
  // in the float mesh but is inside the native seed coalescing tolerance.
  replace(definitions, "((0.,0.,0.))", "((0.,-6.,0.))");
  replace(definitions, "((8.,0.,0.))", "((8.,-6.,0.))");
  replace(definitions, "((8.,6.,0.))", "((8.,0.,0.))");
  replace(definitions, "((8.,12.,8.))", "((8.,6.,8.))");
  definitions += "#25=IFCPOINTBYDISTANCEEXPRESSION(IFCPARAMETERVALUE(2."
                 "00000004),$,$,$,#4);"
                 "#26=IFCAXIS2PLACEMENTLINEAR(#25,#9,$);";
  replace(definitions, "(#22,#23),(#30,#30)", "(#22,#26,#23),(#30,#30,#30)");
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(definitions, "#40", "'Surface','SectionedSurface'"));
  EXPECT_NEAR(checkSectionedMesh(model), 48., 1e-5);
  const auto points = sectionedPoints(model);
  for (float x : {7.f, 9.f}) {
    EXPECT_TRUE(points.contains({x, 0, 0}));
    EXPECT_TRUE(std::ranges::any_of(points, [&](const auto &p) {
      return p[0] == x && p[1] > 0 && p[2] > 0 &&
             std::abs(p[1] - 2.4e-7f) < 1e-12f &&
             std::abs(p[2] - 3.2e-7f) < 1e-12f;
    }));
  }
}

TEST(IfcCurveFamilies, SectionedArcToLineRetainsCenterlineAndSharesMiter) {
  const auto model = container::geometry::ifc::LoadFromStep(
      fixture(curvedSharpSections, "#40", "'Surface','SectionedSurface'"));
  EXPECT_GT(checkSectionedMesh(model), 30.);
  const auto rowPoints = sectionedPoints(model);
  for (float r : {-1.f, 1.f}) {
    // Solve the intersection of the two lateral offset tangent lines.
    const glm::vec3 expected(12 - r, 12 + r, 0);
    EXPECT_TRUE(std::ranges::any_of(rowPoints, [&](const auto &p) {
      return glm::length(glm::dvec3(p[0], p[1], p[2]) - glm::dvec3(expected)) <
             1e-6;
    }));
    const auto line = sectionedColumn(curvedSharpSections, (r + 1.) / 2, 2.);
    ASSERT_FALSE(line.empty());
    EXPECT_TRUE(std::ranges::any_of(line, [&](const auto &p) {
      return glm::length(p - glm::dvec3(expected)) < 1e-6;
    }));
  }
  const auto center = sectionedColumn(curvedSharpSections, .5, 2.);
  ASSERT_FALSE(center.empty());
  EXPECT_LT(glm::length(center.front()), 1e-6);
  EXPECT_LT(glm::length(center.back() - glm::dvec3(24, 12, 0)), 1e-6);
  EXPECT_NEAR(length(center), 6 * pi + 12, .003);
  const glm::dvec3 midpoint(6 * std::sqrt(2.), 12 - 6 * std::sqrt(2.), 0);
  const auto midpointStart =
      curvedSharpSections +
      "#41=IFCCARTESIANPOINT((0.5,0.5)); #42=IFCCARTESIANPOINT((0.75,0.5));"
      "#43=IFCPOLYLINE((#41,#42)); #10=IFCPCURVE(#40,#43);";
  const auto midpointLine =
      container::geometry::ifc::LoadFromStep(fixture(midpointStart));
  complete(midpointLine);
  const auto midpointPoints = vertices(midpointLine);
  ASSERT_FALSE(midpointPoints.empty());
  EXPECT_LT(glm::length(midpointPoints.front() - midpoint), 1e-6);
  for (const auto &p : center) {
    if (p.x < 12 - 1e-6)
      EXPECT_NEAR(std::hypot(p.x, p.y - 12), 12., 1e-6);
    else
      EXPECT_NEAR(p.y, 12., 1e-6);
    EXPECT_NEAR(p.z, 0., 1e-6);
  }
}

TEST(IfcCurveFamilies, InvalidCurvedAndNonplanarSectionedJoinsAreAtomic) {
  auto tightRadius = curvedSharpSections;
  replace(tightRadius, "((-1.,0.))", "((-12.,0.))");
  replace(tightRadius, ".T.,(2.),(0.)", ".T.,(24.),(0.)");

  auto folded = nonplanarSharpSections;
  replace(folded, "((-1.,0.))", "((-7.,0.))");
  replace(folded, ".T.,(2.),(0.)", ".T.,(14.),(0.)");

  auto twisted = nonplanarSharpSections;
  twisted += "#33=IFCDIRECTION((1.,1.,0.)); #34=IFCDIRECTION((1.,0.6,0.8));";
  replace(twisted, "(#20,#8,$)", "(#20,#8,#33)");
  replace(twisted, "(#21,#9,$)", "(#21,#9,#34)");

  auto zeroSpeed = curvedSharpSections;
  // Incoming C(t)=(8(2t−t²),0,0). Direction tends to +X, but the
  // authored endpoint derivative is zero exactly where it turns into +Y.
  zeroSpeed +=
      "#50=IFCCARTESIANPOINT((0.,0.,0.)); #51=IFCCARTESIANPOINT((8.,0.,0.));"
      "#52=IFCBSPLINECURVEWITHKNOTS(2,(#50,#51,#51),.UNSPECIFIED.,.F.,.F.,"
      "(3,3),(0.,1.),.UNSPECIFIED.);";
  replace(zeroSpeed, ".CONTINUOUS.,.T.,#8,1.", ".CONTINUOUS.,.T.,#52,1.");
  replace(zeroSpeed, "((12.,12.,0.))", "((8.,0.,0.))");
  replace(zeroSpeed, "((24.,12.,0.))", "((8.,8.,0.))");

  for (const auto &[source, name] :
       {std::pair{&tightRadius, "zero radius at inner offset"},
        std::pair{&folded, "neighboring miter extensions fold an inner edge"},
        std::pair{&twisted, "incompatible authored section normal"},
        std::pair{&zeroSpeed, "vanishing incident derivative"}}) {
    SCOPED_TRACE(name);
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(*source, "#40", "'Surface','SectionedSurface'"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    EXPECT_TRUE(model.meshRanges.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 40u);
    EXPECT_FALSE(model.importReport.diagnostics.front().reason.empty());
  }
}

TEST(IfcCurveFamilies, SectionedSharpJoinsRejectNonadjacentStripOverlap) {
  for (bool intersects : {false, true}) {
    SCOPED_TRACE(intersects);
    auto definitions = nonplanarSharpSections;
    // Neither directrix intersects itself. The final eastbound strip runs
    // parallel to the first with a gap of 2.5 m, or overlaps it at 1.5 m.
    const std::string y = intersects ? "-1.5" : "-2.5";
    definitions +=
        "#14=IFCCARTESIANPOINT((16.,8.,0.)); "
        "#15=IFCCARTESIANPOINT((16.,16.,0.));"
        "#25=IFCCARTESIANPOINT((-8.,16.,0.)); #26=IFCCARTESIANPOINT((-8.," +
        y + ",0.)); #18=IFCCARTESIANPOINT((8.," + y + ",0.));";
    replace(definitions, "((#2,#3,#5,#6))", "((#2,#3,#5,#14,#15,#25,#26,#18))");
    replace(definitions, "#5=IFCCARTESIANPOINT((8.,6.,0.))",
            "#5=IFCCARTESIANPOINT((8.,8.,0.))");
    replace(definitions, "IFCPARAMETERVALUE(3.)", "IFCPARAMETERVALUE(7.)");
    replace(definitions, "((0.,-0.8,0.6))", "((0.,0.,1.))");
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(definitions, "#40", "'Surface','SectionedSurface'"));
    if (!intersects) {
      // Axis-aligned centerline span lengths sum to 90.5 m, so the
      // two-metre-wide strip has 181 m² of area after its miter cuts.
      EXPECT_NEAR(checkSectionedMesh(model), 181., 1e-5);
      continue;
    }
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 40u);
    EXPECT_NE(model.importReport.diagnostics.front().reason.find(
                  "surface intersects itself"),
              std::string::npos)
        << model.importReport.diagnostics.front().reason;
  }
}

TEST(IfcCurveFamilies, MalformedBranchesAndUnsafeMitersRejectAtomically) {
  std::vector<std::string> invalid;
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"('left','branch','branch','right')",
            "('left','branch','right','branch')"},
           {"('left','branch','branch','right')",
            "('left','new','branch','right')"},
           {"('left','branch','branch','right')", "$"}}) {
    auto definitions = branchingSectionedFixture;
    replace(definitions, from, to);
    invalid.push_back(definitions);
  }
  auto crossing = miterSectionedFixture;
  crossing += "#6=IFCCARTESIANPOINT((0.,10.,0.));"
              "#7=IFCCARTESIANPOINT((0.,-10.,0.));";
  replace(crossing, "IFCPOLYLINE((#2,#3,#5))", "IFCPOLYLINE((#2,#3,#5,#6,#7))");
  replace(crossing, "IFCLENGTHMEASURE(20.)", "IFCPARAMETERVALUE(4.)");
  invalid.push_back(crossing);
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"((10.,10.,0.))", "((0.,0.1,0.))"},
           {"(2.,2.),(0.,0.)", "(20.,20.),(0.,0.)"}}) {
    auto definitions = miterSectionedFixture;
    replace(definitions, from, to);
    if (from == "(2.,2.),(0.,0.)")
      replace(definitions, "((-2.,0.))", "((-20.,0.))");
    // Parameter positions let these malformed directrices reach join
    // validation.
    replace(definitions, "IFCLENGTHMEASURE(20.)", "IFCPARAMETERVALUE(2.)");
    invalid.push_back(definitions);
  }
  for (const auto &definitions : invalid) {
    const auto model =
        container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed)
        << definitions;
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 40u);
    EXPECT_FALSE(model.importReport.diagnostics.front().reason.empty());
  }
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

const std::string derivedEllipseFixture =
    "#2=IFCCARTESIANPOINT((1.,2.)); #3=IFCDIRECTION((0.,1.));"
    "#4=IFCAXIS2PLACEMENT2D(#2,#3);"
    "#5=IFCCIRCLEPROFILEDEF(.CURVE.,$,#4,2.);"
    "#6=IFCCARTESIANPOINT((4.,-1.)); #7=IFCDIRECTION((1.,0.));"
    "#8=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM(#3,#7,#6,3.,0.5);"
    "#9=IFCDERIVEDPROFILEDEF(.CURVE.,$,#5,#8,$);"
    "#11=IFCMIRROREDPROFILEDEF(.CURVE.,$,#9,*,$);"
    "#20=IFCCARTESIANPOINT((10.,20.,30.));"
    "#21=IFCDIRECTION((0.,1.,0.)); #22=IFCDIRECTION((0.,0.,1.));"
    "#23=IFCAXIS2PLACEMENT3D(#20,$,#21);"
    "#24=IFCCARTESIANPOINT((0.,0.,0.)); #25=IFCAXIS1PLACEMENT(#24,$);";

TEST(IfcCurveFamilies,
     DerivedAndMirroredSweptProfilesPreservePlacedParameters) {
  for (bool mirror : {false, true}) {
    for (bool revolution : {false, true}) {
      const std::string parent = mirror ? "#11" : "#9";
      const auto source = fixture(
          derivedEllipseFixture +
          (revolution ? "#30=IFCSURFACEOFREVOLUTION(" + parent + ",#23,#25);"
                      : "#30=IFCSURFACEOFLINEAREXTRUSION(" + parent +
                            ",#23,#22,2.);") +
          "#31=IFCCARTESIANPOINT((0.,0.));"
          "#32=IFCCARTESIANPOINT((6.283185307179586," +
          std::string(revolution ? "0." : "1.") +
          ")); #33=IFCPOLYLINE((#31,#32)); #10=IFCPCURVE(#30,#33);");
      const auto model = container::geometry::ifc::LoadFromStep(source);
      complete(model);
      const auto points = vertices(model);
      ASSERT_FALSE(points.empty());
      EXPECT_EQ(points.front(), glm::dvec3(8, mirror ? 14 : 26, 30));
      EXPECT_NEAR(points.back().z, revolution ? 30 : 32, 1e-6);
      for (const auto p : points) {
        if (revolution) {
          EXPECT_NEAR((p.x - 10) * (p.x - 10) + (p.y - 20) * (p.y - 20), 40,
                      4e-5);
          EXPECT_EQ(p.z, 30);
        } else {
          const double cy = mirror ? 15 : 25;
          EXPECT_NEAR((p.x - 8) * (p.x - 8) / 36 + (p.y - cy) * (p.y - cy), 1,
                      4e-6);
        }
      }
      if (!revolution)
        exportProfileFixture(mirror ? "mirrored-ellipse" : "derived-ellipse",
                             source);
    }
  }
}

TEST(IfcCurveFamilies, DerivedProfileAxesHonorDefaultsAndPerpendicularSenses) {
  const std::string definitions =
      "#2=IFCCARTESIANPOINT((1.,2.)); #3=IFCCARTESIANPOINT((3.,4.));"
      "#4=IFCPOLYLINE((#2,#3)); #5=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#4);"
      "#6=IFCCARTESIANPOINT((10.,20.)); #7=IFCDIRECTION((3.,4.));"
      "#8=IFCDIRECTION((-4.,3.)); #11=IFCDIRECTION((4.,0.));"
      "#20=IFCDIRECTION((0.,0.,1.));"
      "#14=IFCDERIVEDPROFILEDEF(.CURVE.,$,#5,#9,$);"
      "#30=IFCSURFACEOFLINEAREXTRUSION(#14,$,#20,1.);"
      "#31=IFCCARTESIANPOINT((0.,0.)); #32=IFCCARTESIANPOINT((1.,0.));"
      "#33=IFCPOLYLINE((#31,#32)); #10=IFCPCURVE(#30,#33);";
  struct Case {
    std::string op;
    glm::dvec3 first, last;
  };
  for (const auto &test : std::vector<Case>{
           {"IFCCARTESIANTRANSFORMATIONOPERATOR2D($,$,#6,$)",
            {11, 22, 0},
            {13, 24, 0}},
           {"IFCCARTESIANTRANSFORMATIONOPERATOR2D(#7,$,#6,2.)",
            {8, 24, 0},
            {7.2, 29.6, 0}},
           {"IFCCARTESIANTRANSFORMATIONOPERATOR2D($,#8,#6,2.)",
            {8, 24, 0},
            {7.2, 29.6, 0}},
           {"IFCCARTESIANTRANSFORMATIONOPERATOR2D(#7,#11,#6,2.)",
            {14.4, 19.2, 0},
            {20, 20, 0}},
           {"IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#6,2.,$)",
            {12, 24, 0},
            {16, 28, 0}},
           {"IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#6,$,3.)",
            {11, 26, 0},
            {13, 32, 0}}}) {
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(definitions + "#9=" + test.op + ";"));
    complete(model);
    const auto points = vertices(model);
    ASSERT_FALSE(points.empty());
    EXPECT_LT(glm::length(points.front() - test.first), 2e-6);
    EXPECT_LT(glm::length(points.back() - test.last), 2e-6);
  }
  auto largeSource = definitions;
  replace(largeSource, "((1.,2.))", "((1.E18,2.E18))");
  replace(largeSource, "((3.,4.))", "((3.E18,4.E18))");
  largeSource += "#9=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#6,1.E-"
                 "18,2.E-18);";
  const auto reduced =
      container::geometry::ifc::LoadFromStep(fixture(largeSource));
  complete(reduced);
  const auto points = vertices(reduced);
  ASSERT_FALSE(points.empty());
  EXPECT_EQ(points.front(), glm::dvec3(11, 24, 0));
  EXPECT_EQ(points.back(), glm::dvec3(13, 28, 0));
}

TEST(IfcCurveFamilies, ScaledSplineProfilesKeepChordAccuracyAndSurfaceInverse) {
  auto definitions = sectionedFixture;
  replace(definitions, "(#30,#31)", "(#60,#60)");
  definitions +=
      "#50=IFCCARTESIANPOINT((1.,0.)); #51=IFCCARTESIANPOINT((1.,1.));"
      "#52=IFCCARTESIANPOINT((0.,1.));"
      "#53=IFCRATIONALBSPLINECURVEWITHKNOTS(2,(#50,#51,#52),.UNSPECIFIED.,"
      ".F.,.F.,(3,3),(2.,4.),.UNSPECIFIED.,(1.,0.7071067811865476,1.));"
      "#54=IFCARBITRARYOPENPROFILEDEF(.CURVE.,$,#53);"
      "#55=IFCCARTESIANPOINT((0.,0.));"
      "#56=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#55,100.,1.);"
      "#60=IFCDERIVEDPROFILEDEF(.CURVE.,$,#54,#56,$);";
  const auto model =
      container::geometry::ifc::LoadFromStep(fixture(definitions, "#40"));
  checkSectionedMesh(model, {0, 0, -1});
  std::map<double, glm::dvec2> boundary;
  for (const auto &vertex : model.vertices) {
    const auto p = vertex.position;
    EXPECT_NEAR(double(p.y) * p.y / 10000 + double(p.z) * p.z, 1, 2e-7);
    if (p.x == 0)
      boundary[std::atan2(p.z, p.y / 100.)] = {p.y, p.z};
  }
  ASSERT_GT(boundary.size(), 2u);
  auto previous = boundary.begin();
  for (auto current = std::next(previous); current != boundary.end();
       ++current) {
    const double angle = (current->first + previous->first) / 2;
    const glm::dvec2 exact(100 * std::cos(angle), std::sin(angle));
    const auto a = previous->second, d = current->second - a;
    const auto nearest =
        a + d * std::clamp(glm::dot(exact - a, d) / glm::dot(d, d), 0., 1.);
    EXPECT_LE(glm::length(exact - nearest), .001);
    previous = current;
  }

  SweptChartEntities step;
  using S = SweptChartEntities;
  step.point(1, {0, 0});
  step.add(2, "IFCAXIS2PLACEMENT2D", {S::ref(1), {}});
  step.add(3, "IFCCIRCLE", {S::ref(2), S::number(1)});
  step.add(4, "IFCARBITRARYCLOSEDPROFILEDEF", {S::curve(), {}, S::ref(3)});
  step.point(5, {10, 20});
  step.add(6, "IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM",
           {{}, {}, S::ref(5), S::number(100), S::number(1)});
  step.add(7, "IFCDERIVEDPROFILEDEF",
           {S::curve(), {}, S::ref(4), S::ref(6), {}});
  step.add(8, "IFCMIRROREDPROFILEDEF", {S::curve(), {}, S::ref(7), {}, {}});
  step.direction(9, {0, 0, 1});
  step.add(10, "IFCSURFACEOFLINEAREXTRUSION",
           {S::ref(8), {}, S::ref(9), S::number(3)});
  std::string error;
  const auto surface = readIfcSurface(step.entities, 10, 1, 1, error);
  ASSERT_TRUE(surface) << error;
  ASSERT_TRUE(surface->inverse);
  const auto p = surface->point(.7, 1.2);
  ASSERT_TRUE(p);
  EXPECT_NEAR(p->x, -10 - 100 * std::cos(.7), 1e-12);
  EXPECT_NEAR(p->y, 20 + std::sin(.7), 1e-12);
  const auto uv = surface->inverse(*p);
  ASSERT_TRUE(uv);
  EXPECT_NEAR(uv->x, .7, 1e-12);
  EXPECT_NEAR(uv->y, 1.2, 1e-12);
}

TEST(IfcCurveFamilies, DerivedSectionedProfilesRetainTagsAndMiterOrientation) {
  for (bool sharp : {false, true}) {
    for (bool mirror : {false, true}) {
      auto definitions =
          sharp ? miterSectionedFixture : branchingSectionedFixture;
      if (sharp) {
        definitions +=
            "#31=IFCOPENCROSSPROFILEDEF(.CURVE.,$,.T.,(1.,2.,1.),(0.,0.,0.),"
            "('left','crown','crown','right'),#24);";
        replace(definitions, "(#30,#30)", "(#30,#31)");
      }
      definitions +=
          "#62=IFCCARTESIANPOINT((0.,0.));"
          "#63=IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM($,$,#62,1.5,0.5);"
          "#60=IFCDERIVEDPROFILEDEF(.CURVE.,$,#30,#63,$);"
          "#61=IFCDERIVEDPROFILEDEF(.CURVE.,$,#31,#63,$);"
          "#64=IFCMIRROREDPROFILEDEF(.CURVE.,$,#60,*,$);"
          "#65=IFCMIRROREDPROFILEDEF(.CURVE.,$,#61,*,$);";
      replace(definitions, "(#30,#31)", mirror ? "(#64,#65)" : "(#60,#61)");
      const auto source = fixture(definitions, "#40");
      const auto model = container::geometry::ifc::LoadFromStep(source);
      EXPECT_NEAR(checkSectionedMesh(model, {0, 0, mirror ? -1 : 1}),
                  sharp ? 120 : 60, 1e-6);
      if (mirror == sharp)
        exportProfileFixture(sharp ? "mirrored-miter" : "derived-section",
                             source);
    }
  }
}

TEST(IfcCurveFamilies, InvalidAndRecursiveDerivedProfilesRejectAtomically) {
  std::vector<std::string> sources;
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"#5,#8,$", "#9,#8,$"},
           {"#5,#8,$", "#99,#8,$"},
           {"#5,#8,$", "#11,#8,$"},
           {"IFCDERIVEDPROFILEDEF(.CURVE.", "IFCDERIVEDPROFILEDEF(.AREA."},
           {"#6,3.,0.5", "#6,0.,0.5"},
           {"#6,3.,0.5", "#6,3.,-0.5"},
           {"IFCCARTESIANPOINT((4.,-1.))", "IFCCARTESIANPOINT((4.,-1.,0.))"},
           {"IFCDIRECTION((1.,0.))", "IFCDIRECTION((0.,0.))"},
           {"IFCDIRECTION((1.,0.))", "IFCDIRECTION((1.,0.,0.))"},
           {"IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM",
            "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM"}}) {
    auto definitions = derivedEllipseFixture;
    replace(definitions, from, to);
    sources.push_back(definitions);
  }
  auto excessive = derivedEllipseFixture;
  for (unsigned i = 100; i < 170; ++i)
    excessive += "#" + std::to_string(i) + "=IFCDERIVEDPROFILEDEF(.CURVE.,$,#" +
                 std::to_string(i == 169 ? 5 : i + 1) + ",#8,$);";
  replace(excessive, "#5,#8,$", "#100,#8,$");
  sources.push_back(excessive);
  for (const auto &definitions : sources) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        definitions +
        "#30=IFCSURFACEOFLINEAREXTRUSION(#9,#23,#22,2.);"
        "#31=IFCCARTESIANPOINT((0.,0.)); #32=IFCCARTESIANPOINT((1.,1.));"
        "#33=IFCPOLYLINE((#31,#32)); #10=IFCPCURVE(#30,#33);"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_TRUE(model.indices.empty());
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 10u);
    EXPECT_FALSE(model.importReport.diagnostics.front().reason.empty());
  }
}

const std::string halfspaceFixture =
    "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
    "#4=IFCDIRECTION((0.,0.,1.)); #5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.);"
    "#6=IFCEXTRUDEDAREASOLID(#5,#3,#4,4.);"
    "#11=IFCCARTESIANPOINT((0.,0.,2.)); #12=IFCDIRECTION((1.,0.,1.));"
    "#13=IFCAXIS2PLACEMENT3D(#11,#12,$); #14=IFCPLANE(#13);"
    "#20=IFCCARTESIANPOINT((0.,-1.)); #21=IFCCARTESIANPOINT((2.,-1.));"
    "#22=IFCCARTESIANPOINT((2.,1.)); #23=IFCCARTESIANPOINT((0.,1.));"
    "#24=IFCPOLYLINE((#20,#21,#22,#23,#20));";

TEST(IfcCurveFamilies, PolygonalHalfSpacesHonorObliquePlanesAndAgreement) {
  for (bool difference : {false, true})
    for (bool agreement : {false, true}) {
      const auto model = container::geometry::ifc::LoadFromStep(
          fixture(halfspaceFixture + "#30=IFCPOLYGONALBOUNDEDHALFSPACE(#14,." +
                  std::string(agreement ? "T" : "F") +
                  ".,#3,#24); #10=IFCBOOLEANRESULT(." +
                  std::string(difference ? "DIFFERENCE" : "INTERSECTION") +
                  ".,#6,#30);"));
      for (const auto &d : model.importReport.diagnostics)
        ADD_FAILURE() << d.reason;
      ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
      const double cutVolume = agreement ? 4 : 12;
      EXPECT_NEAR(volume(model), difference ? 64 - cutVolume : cutVolume, 2e-5);
      watertight(model);
    }
}

TEST(IfcCurveFamilies,
     PolygonalHalfSpacesRetainRotatedBoundaryFramesAndWinding) {
  for (bool reversed : {false, true}) {
    auto definitions = halfspaceFixture;
    if (reversed) {
      const std::string original = "#20,#21,#22,#23,#20";
      definitions.replace(definitions.find(original), original.size(),
                          "#20,#23,#22,#21,#20");
    }
    const auto model = container::geometry::ifc::LoadFromStep(fixture(
        definitions +
        "#31=IFCCARTESIANPOINT((1.,0.,0.)); #32=IFCDIRECTION((0.,1.,0.));"
        "#33=IFCAXIS2PLACEMENT3D(#31,$,#32);"
        "#30=IFCPOLYGONALBOUNDEDHALFSPACE(#14,.F.,#33,#24);"
        "#10=IFCBOOLEANRESULT(.DIFFERENCE.,#6,#30);"));
    ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(volume(model), 52, 2e-5);
    watertight(model);
  }
}

TEST(IfcCurveFamilies, BoxedHalfSpaceSearchHintsDoNotClipTheResult) {
  const auto model = container::geometry::ifc::LoadFromStep(fixture(
      halfspaceFixture +
      "#31=IFCBOUNDINGBOX(#2,0.1,0.1,0.1); #30=IFCBOXEDHALFSPACE(#14,.F.,#31);"
      "#10=IFCBOOLEANRESULT(.INTERSECTION.,#6,#30);"));
  for (const auto &d : model.importReport.diagnostics)
    ADD_FAILURE() << d.reason;
  ASSERT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_NEAR(volume(model), 32, 1e-5);
  watertight(model);
}

TEST(IfcCurveFamilies, InvalidBoundedHalfSpacesRejectAtomically) {
  for (const std::string operand :
       {"#30=IFCPOLYGONALBOUNDEDHALFSPACE(#14,.F.,#3,#4);",
        "#31=IFCBOUNDINGBOX(#2,-1.,1.,1.); #30=IFCBOXEDHALFSPACE(#14,.F.,#31);",
        "#31=IFCDIRECTION((1.,0.,0.)); #32=IFCAXIS2PLACEMENT3D(#11,#31,$);"
        "#33=IFCPLANE(#32); "
        "#30=IFCPOLYGONALBOUNDEDHALFSPACE(#33,.F.,#3,#24);"}) {
    const auto model = container::geometry::ifc::LoadFromStep(
        fixture(halfspaceFixture + operand +
                "#10=IFCBOOLEANRESULT(.DIFFERENCE.,#6,#30);"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_TRUE(model.vertices.empty());
  }
}

TEST(IfcCurveFamilies, BuildingSmartCivilSamplesHaveDrawableAlignmentCurves) {
  const char *root = std::getenv("CONTAINER_IFC_SAMPLE_ROOT");
  if (!root)
    GTEST_SKIP() << "Set CONTAINER_IFC_SAMPLE_ROOT for IFC4.3 road/rail "
                    "alignment checks";
  for (const std::string sample : {"Railway/Railway_project_simple_IFC4X3.ifc",
                                   "Road/Roadway_project_IFC4X3.ifc"}) {
    std::ifstream input(std::filesystem::path(root) / sample);
    ASSERT_TRUE(input);
    std::string definitions, line;
    std::vector<std::string> curves;
    while (std::getline(input, line)) {
      const auto equal = line.find('=');
      if (equal == std::string::npos)
        continue;
      const auto end = line.find('(', equal);
      if (end == std::string::npos)
        continue;
      const auto type = line.substr(equal + 1, end - equal - 1);
      if (type == "IFCGRADIENTCURVE")
        curves.push_back(line.substr(0, equal));
      if (container::geometry::ifc::detail::isCurveEntity(type) ||
          type == "IFCCOMPOSITECURVESEGMENT" ||
          type == "IFCREPARAMETRISEDCOMPOSITECURVESEGMENT" ||
          type == "IFCCARTESIANPOINT" || type == "IFCCARTESIANPOINTLIST2D" ||
          type == "IFCCARTESIANPOINTLIST3D" || type == "IFCDIRECTION" ||
          type == "IFCVECTOR" || type == "IFCAXIS2PLACEMENT2D" ||
          type == "IFCAXIS2PLACEMENT3D" || type == "IFCSIUNIT")
        definitions += line;
    }
    ASSERT_FALSE(curves.empty()) << sample;
    // Fixtures intentionally remove solid/product representations so failures
    // identify the alignment itself, independent of remaining solid coverage.
    for (const auto &curve : curves) {
      const auto model =
          container::geometry::ifc::LoadFromStep(fixture(definitions, curve));
      complete(model);
      const auto p = vertices(model);
      ASSERT_GT(p.size(), 20u) << sample << " " << curve;
      for (auto v : p)
        ASSERT_TRUE(std::isfinite(v.x) && std::isfinite(v.y) &&
                    std::isfinite(v.z));
    }
  }
}
} // namespace
