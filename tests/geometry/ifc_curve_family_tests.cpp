#include "../../src/geometry/IfcCurveReader.h"
#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/ParametricCurve.h"
#include "Container/geometry/SweptDiskGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <gtest/gtest.h>
#include <map>
#include <numbers>
#include <set>
#include <sstream>

namespace {
using container::geometry::ImportCompleteness;
using container::geometry::ifc::Model;
constexpr double pi = std::numbers::pi;

std::string fixture(std::string_view definitions,
                    std::string_view items = "#10") {
  return "ISO-10303-21; DATA; #1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);" +
         std::string(definitions) +
         "#900001=IFCSHAPEREPRESENTATION($,'Axis','Curve3D',(" +
         std::string(items) +
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
           {"(2.,2.),(0.,0.)", "(20.,20.),(0.,0.)"},
           {"((10.,10.,0.))", "((10.,10.,1.))"}}) {
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

  using container::geometry::ifc::detail::Entity;
  using container::geometry::ifc::detail::StepValue;
  const auto numeric = [](double n) {
    StepValue v;
    v.kind = StepValue::Kind::Number;
    v.number = n;
    return v;
  };
  const auto ref = [](uint32_t id) {
    StepValue v;
    v.kind = StepValue::Kind::Ref;
    v.ref = id;
    return v;
  };
  const auto values = [](std::initializer_list<StepValue> a) {
    StepValue v;
    v.kind = StepValue::Kind::List;
    v.list = a;
    return v;
  };
  StepValue curveType;
  curveType.kind = StepValue::Kind::Enum;
  curveType.text = "CURVE";
  std::unordered_map<uint32_t, Entity> entities;
  const auto add = [&](uint32_t id, const char *type,
                       std::initializer_list<StepValue> a) {
    entities.emplace(id, Entity{id, type, values(a)});
  };
  add(1, "IFCCARTESIANPOINT", {values({numeric(0), numeric(0)})});
  add(2, "IFCAXIS2PLACEMENT2D", {ref(1), {}});
  add(3, "IFCCIRCLE", {ref(2), numeric(1)});
  add(4, "IFCARBITRARYCLOSEDPROFILEDEF", {curveType, {}, ref(3)});
  add(5, "IFCCARTESIANPOINT", {values({numeric(10), numeric(20)})});
  add(6, "IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM",
      {{}, {}, ref(5), numeric(100), numeric(1)});
  add(7, "IFCDERIVEDPROFILEDEF", {curveType, {}, ref(4), ref(6), {}});
  add(8, "IFCMIRROREDPROFILEDEF", {curveType, {}, ref(7), {}, {}});
  add(9, "IFCDIRECTION", {values({numeric(0), numeric(0), numeric(1)})});
  add(10, "IFCSURFACEOFLINEAREXTRUSION", {ref(8), {}, ref(9), numeric(3)});
  std::string error;
  const auto surface = container::geometry::ifc::detail::readIfcSurface(
      entities, 10, 1, 1, error);
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
