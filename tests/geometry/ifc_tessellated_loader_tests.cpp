#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/geometry/ParametricCurve.h"
#include "Container/geometry/PolylineGeometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#ifndef CONTAINER_BINARY_DIR
#define CONTAINER_BINARY_DIR "."
#endif

namespace {

using container::geometry::ImportCompleteness;
using IfcModel = container::geometry::ifc::Model;

std::string polygonFixture(std::string_view points, std::string_view face,
                           std::string_view pnIndex = "$",
                           std::string_view extra = "") {
  return "ISO-10303-21; DATA; #1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);"
         "#20=IFCCARTESIANPOINTLIST3D(" +
         std::string(points) +
         ");"
         "#21=IFCPOLYGONALFACESET(#20,.F.,(#22)," +
         std::string(pnIndex) +
         ");"
         "#22=" +
         std::string(face) +
         ";"
         "#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));"
         "#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));"
         "#40=IFCBUILDINGELEMENTPROXY('polygon-guid',$,'Polygon',$,$,$,#31,$,$)"
         ";" +
         std::string(extra) + " ENDSEC; END-ISO-10303-21;";
}

double meshArea(const IfcModel &model) {
  double area = 0;
  for (size_t i = 0; i + 2u < model.indices.size(); i += 3u) {
    const glm::dvec3 a(model.vertices[model.indices[i]].position);
    const glm::dvec3 b(model.vertices[model.indices[i + 1u]].position);
    const glm::dvec3 c(model.vertices[model.indices[i + 2u]].position);
    area += glm::length(glm::cross(b - a, c - a)) * 0.5;
  }
  return area;
}

double signedMeshVolume(const IfcModel &model) {
  double volume = 0;
  for (size_t i = 0; i + 2u < model.indices.size(); i += 3u) {
    const glm::dvec3 a(model.vertices[model.indices[i]].position);
    const glm::dvec3 b(model.vertices[model.indices[i + 1u]].position);
    const glm::dvec3 c(model.vertices[model.indices[i + 2u]].position);
    volume += glm::dot(a, glm::cross(b, c)) / 6.0;
  }
  return volume;
}

std::string solidFixture(std::string_view entities,
                         std::string_view solid = "#10") {
  return "ISO-10303-21; DATA; #1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);" +
         std::string(entities) +
         "#90=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(" +
         std::string(solid) +
         ")); #91=IFCPRODUCTDEFINITIONSHAPE($,$,(#90));"
         "#92=IFCBUILDINGELEMENTPROXY('solid-guid',$,'Solid',$,$,$,#91,$,$);"
         "ENDSEC; END-ISO-10303-21;";
}

double productVolume(const IfcModel &model) {
  double volume = 0;
  for (const auto &element : model.elements) {
    if (element.geometryKind != container::geometry::dotbim::GeometryKind::Mesh)
      continue;
    const auto range =
        std::ranges::find(model.meshRanges, element.meshId,
                          &container::geometry::dotbim::MeshRange::meshId);
    if (range == model.meshRanges.end())
      continue;
    for (size_t i = range->firstIndex;
         i < size_t(range->firstIndex) + range->indexCount; i += 3) {
      const glm::dvec3 a(model.vertices[model.indices[i]].position),
          b(model.vertices[model.indices[i + 1]].position),
          c(model.vertices[model.indices[i + 2]].position);
      volume += glm::dot(a, glm::cross(b, c)) / 6;
    }
  }
  return volume;
}

std::vector<glm::vec3> nativeCurveEndpoints(const IfcModel &model) {
  std::vector<glm::vec3> result;
  for (const auto &range : model.nativeCurveRanges)
    for (size_t i = range.firstIndex;
         i < size_t(range.firstIndex) + range.indexCount; ++i)
      result.push_back(model.vertices.at(model.indices.at(i)).position);
  return result;
}

TEST(ParametricCurve, BezierTessellationBoundsSCurveAndRationalCircleError) {
  using namespace container::geometry;
  const auto spline =
      makeBSplineCurve(3, {{0, 0, 0}, {1, 3, 0}, {2, -3, 0}, {3, 0, 0}},
                       {0, 0, 0, 0, 1, 1, 1, 1});
  ASSERT_TRUE(spline);
  const auto samples = sampleCurve(*spline, 0, 1, .001);
  ASSERT_GT(samples.size(), 4u);
  for (size_t i = 1; i < samples.size(); ++i)
    for (unsigned j = 0; j <= 100; ++j) {
      const double t =
          samples[i - 1].parameter +
          (samples[i].parameter - samples[i - 1].parameter) * j / 100;
      const glm::dvec3 analytic(3 * t, 9 * t * (1 - t) * (1 - 2 * t), 0);
      ASSERT_TRUE(spline->point(t));
      EXPECT_NEAR(glm::length(*spline->point(t) - analytic), 0, 1e-12);
      const auto a = samples[i - 1].point, b = samples[i].point, d = b - a;
      const double q = glm::dot(d, d);
      EXPECT_LE(
          glm::length(analytic - a -
                      d * std::clamp(glm::dot(analytic - a, d) / q, 0.0, 1.0)),
          .001 + 1e-12);
    }
  const auto circle =
      makeBSplineCurve(2, {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {0, 0, 0, 1, 1, 1},
                       {1, std::sqrt(.5), 1});
  ASSERT_TRUE(circle);
  for (double t : {0., .1, .5, .9, 1.}) {
    ASSERT_TRUE(circle->point(t));
    EXPECT_NEAR(glm::length(*circle->point(t)), 1, 1e-12);
  }
  const auto trimmed = sampleCurve(*circle, .8, .2, .001);
  ASSERT_GT(trimmed.size(), 2u);
  EXPECT_NEAR(trimmed.front().parameter, .8, 1e-12);
  EXPECT_NEAR(trimmed.back().parameter, .2, 1e-12);
  EXPECT_NEAR(glm::length(trimmed.front().point - *circle->point(.8)), 0,
              1e-12);
  EXPECT_NEAR(glm::length(trimmed.back().point - *circle->point(.2)), 0, 1e-12);
  const auto inverse =
      curveParameterAtPoint(*circle, *circle->point(.37), 1e-7);
  ASSERT_TRUE(inverse);
  EXPECT_NEAR(*inverse, .37, 1e-7);
}

TEST(ParametricCurve, UniformUnclampedKnotsRetainActiveDomainAndEndpoints) {
  using namespace container::geometry;
  const std::vector<glm::dvec3> controls{
      {0, 0, 0}, {1, 2, 0}, {3, 1, 0}, {5, 3, 0}};
  const std::vector<double> knots{0, 1, 2, 3, 4, 5, 6};
  const auto spline = makeBSplineCurve(2, controls, knots);
  ASSERT_TRUE(spline);
  const auto samples = sampleCurve(*spline, 2, 4, .001);
  ASSERT_GT(samples.size(), 2u);
  EXPECT_NEAR(
      glm::length(samples.front().point - (controls[0] + controls[1]) * .5), 0,
      1e-12);
  EXPECT_NEAR(
      glm::length(samples.back().point - (controls[2] + controls[3]) * .5), 0,
      1e-12);
  // Independent Cox-de Boor basis evaluation, avoiding the implementation's
  // de Boor point evaluator and Bezier subdivision.
  std::function<double(size_t, unsigned, double)> basis;
  basis = [&](size_t i, unsigned p, double t) {
    if (!p)
      return knots[i] <= t && t < knots[i + 1] ? 1. : 0.;
    return (t - knots[i]) / (knots[i + p] - knots[i]) * basis(i, p - 1, t) +
           (knots[i + p + 1] - t) / (knots[i + p + 1] - knots[i + 1]) *
               basis(i + 1, p - 1, t);
  };
  for (const auto &sample : samples) {
    if (sample.parameter == 4)
      continue;
    glm::dvec3 expected(0);
    for (size_t i = 0; i < controls.size(); ++i)
      expected += controls[i] * basis(i, 2, sample.parameter);
    EXPECT_NEAR(glm::length(sample.point - expected), 0, 1e-12);
  }
  EXPECT_TRUE(sampleCurve(*spline, 1, 4, .001).empty());
  EXPECT_TRUE(sampleCurve(*spline, 2, 4, .001, 2).empty());
  EXPECT_FALSE(makeBSplineCurve(2, controls, {0, 0, 0, 1, 1, 1, 1}));
  EXPECT_FALSE(makeBSplineCurve(2, controls, knots, {1, -1, 1, 1}));
}

TEST(IfcTessellatedLoader,
     NativeClosedPolylinePreservesStyleIdentityAndClosure) {
  const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCCARTESIANPOINT((1.,0.,0.));"
      "#4=IFCCARTESIANPOINT((1.,1.,0.)); #5=IFCCARTESIANPOINT((0.,1.,0.));"
      "#10=IFCPOLYLINE((#2,#3,#3,#4,#5,#2));"
      "#30=IFCCOLOURRGB($,1.,0.,0.); #31=IFCCURVESTYLE($,$,$,#30,$);"
      "#32=IFCSTYLEDITEM(#10,(#31),$);"));
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].geometryKind,
            container::geometry::dotbim::GeometryKind::Curves);
  EXPECT_EQ(model.elements[0].guid, "solid-guid");
  EXPECT_EQ(model.elements[0].sourceId, "#92");
  EXPECT_EQ(model.elements[0].color, glm::vec4(1, 0, 0, 1));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  ASSERT_EQ(model.meshRanges.size(), 1u); // Picking proxy remains available.
  ASSERT_EQ(model.nativeCurveRanges.size(), 1u);
  EXPECT_EQ(model.nativeCurveRanges[0].meshId, model.elements[0].meshId);
  const auto points = nativeCurveEndpoints(model);
  ASSERT_EQ(points.size(), 8u);
  EXPECT_EQ(points.front(), points.back());
  double length = 0;
  for (size_t i = 0; i < points.size(); i += 2)
    length += glm::length(points[i + 1] - points[i]);
  EXPECT_NEAR(length, 4, 1e-6);
  for (const auto index : model.indices)
    EXPECT_LT(index, model.vertices.size());
}

TEST(IfcTessellatedLoader, CurveSetsKeepPathsSeparateAndMappedGeometryShared) {
  auto step = solidFixture(
      "#2=IFCCARTESIANPOINTLIST2D(((999.,999.),(0.,0.),(1000.,0.),(1000.,1000.)"
      "));"
      "#3=IFCINDEXEDPOLYCURVE(#2,(IFCLINEINDEX((2,3)),IFCLINEINDEX((3,4,2))),."
      "F.);"
      "#4=IFCCARTESIANPOINTLIST3D(((3000.,0.,0.),(4000.,0.,0.)));"
      "#5=IFCINDEXEDPOLYCURVE(#4,$,.F.); #6=IFCGEOMETRICCURVESET((#3,#5));"
      "#7=IFCSHAPEREPRESENTATION($,'Body','GeometricCurveSet',(#6));"
      "#8=IFCREPRESENTATIONMAP($,#7);"
      "#11=IFCCARTESIANPOINT((5000.,0.,0.));"
      "#12=IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$,#11,1.,$);"
      "#10=IFCMAPPEDITEM(#8,#12); #13=IFCMAPPEDITEM(#8,$);"
      "#30=IFCCOLOURRGB($,0.,1.,0.); #31=IFCCURVESTYLE($,$,$,#30,$);"
      "#32=IFCSTYLEDITEM(#10,(#31),$);",
      "#10,#13");
  const auto units = step.find("$,.METRE.");
  step.replace(units, 9, ".MILLI.,.METRE.");
  const auto model = container::geometry::ifc::LoadFromStep(step);
  ASSERT_EQ(model.elements.size(), 4u);
  ASSERT_EQ(model.nativeCurveRanges.size(), 2u);
  EXPECT_EQ(model.elements[0].meshId, model.elements[2].meshId);
  EXPECT_EQ(model.elements[1].meshId, model.elements[3].meshId);
  EXPECT_EQ(model.elements[0].color, glm::vec4(0, 1, 0, 1));
  EXPECT_NE(model.elements[2].color, glm::vec4(0, 1, 0, 1));
  EXPECT_NEAR(model.elements[0].transform[3].x, 5, 1e-6);
  EXPECT_NEAR(model.elements[2].transform[0].x, .001, 1e-8);
  const auto points = nativeCurveEndpoints(model);
  ASSERT_EQ(points.size(), 8u);
  EXPECT_EQ(points[0], points[5]);
  EXPECT_EQ(points[6], glm::vec3(3000, 0, 0));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
}

TEST(IfcTessellatedLoader, IndexedArcsFollowAuthoredPlaneMiddlePointAndSense) {
  const auto model = container::geometry::ifc::LoadFromStep(
      solidFixture("#2=IFCCARTESIANPOINTLIST3D(((1.,2.,0.),(0.,2.,1.),(-1.,2.,"
                   "0.),(-2.,2.,0.)));"
                   "#10=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((1,2,3)),"
                   "IFCLINEINDEX((3,4))),.F.);"));
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  const auto points = nativeCurveEndpoints(model);
  ASSERT_GT(points.size(), 8u);
  EXPECT_EQ(points.front(), glm::vec3(1, 2, 0));
  EXPECT_EQ(points.back(), glm::vec3(-2, 2, 0));
  EXPECT_NE(std::ranges::find(points, glm::vec3(0, 2, 1)), points.end());
  double length = 0;
  for (size_t i = 0; i < points.size(); i += 2) {
    length += glm::length(points[i + 1] - points[i]);
    EXPECT_NEAR(points[i].y, 2, 1e-6);
    if (i + 2 < points.size()) {
      EXPECT_NEAR(points[i].x * points[i].x + points[i].z * points[i].z, 1,
                  1e-5);
      EXPECT_GE(points[i].z, 0);
    }
  }
  EXPECT_NEAR(length, 1 + std::acos(-1.0), .002);
  const auto collinear = container::geometry::ifc::LoadFromStep(
      solidFixture("#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.,0.),(2.,0.)));"
                   "#10=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((1,2,3))),.F.);"));
  EXPECT_EQ(collinear.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_EQ(nativeCurveEndpoints(collinear).size(), 4u);
}

TEST(IfcTessellatedLoader, MalformedCurvesFailWithoutOrphanBuffers) {
  for (const std::string entities :
       {"#10=IFCPOLYLINE((#999,#998));",
        "#2=IFCCARTESIANPOINT((0.,0.)); #10=IFCPOLYLINE((#2,#2));",
        "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.,0.,0.))); "
        "#10=IFCINDEXEDPOLYCURVE(#2,$,$);",
        "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.,0.))); "
        "#10=IFCINDEXEDPOLYCURVE(#2,(IFCLINEINDEX((0,2))),$);",
        "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.,0.))); "
        "#10=IFCINDEXEDPOLYCURVE(#2,(IFCLINEINDEX((1,3))),$);",
        "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.,0.),(2.,0.),(3.,0.))); "
        "#10=IFCINDEXEDPOLYCURVE(#2,(IFCLINEINDEX((1,2)),IFCLINEINDEX((3,4))),$"
        ");",
        "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.,0.))); "
        "#10=IFCINDEXEDPOLYCURVE(#2,(IFCARCINDEX((1,2))),$);",
        "#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.E100,0.))); "
        "#10=IFCINDEXEDPOLYCURVE(#2,$,$);"}) {
    const auto model =
        container::geometry::ifc::LoadFromStep(solidFixture(entities));
    EXPECT_TRUE(model.elements.empty()) << entities;
    EXPECT_TRUE(model.vertices.empty()) << entities;
    EXPECT_TRUE(model.indices.empty());
    EXPECT_TRUE(model.nativeCurveRanges.empty());
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics[0].entityId, 10u);
    EXPECT_EQ(model.importReport.diagnostics[0].productGuid, "solid-guid");
  }
}

TEST(IfcTessellatedLoader,
     CurveSetKeepsValidCurvesAndReportsUnsupportedChildren) {
  const auto model = container::geometry::ifc::LoadFromStep(
      solidFixture("#2=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.,0.)));"
                   "#3=IFCINDEXEDPOLYCURVE(#2,$,$); #4=IFCBSPLINECURVE();"
                   "#10=IFCGEOMETRICCURVESET((#3,#4));"));
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
  EXPECT_EQ(model.importReport.partialProductCount, 1u);
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  EXPECT_EQ(model.importReport.diagnostics[0].entityId, 4u);
  EXPECT_EQ(model.importReport.diagnostics[0].representationType,
            "IFCBSPLINECURVE");
}

TEST(IfcTessellatedLoader, AuxiliaryCurvesDoNotPreventSolidOpeningSubtraction) {
  for (bool circle : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
        "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.); "
        "#10=IFCEXTRUDEDAREASOLID(#5,$,$,4.);"
        "#11=IFCCARTESIANPOINT((0.,0.,-1.)); #12=IFCAXIS2PLACEMENT3D(#11,$,$);"
        "#15=" +
            std::string(circle ? "IFCCIRCLEPROFILEDEF(.AREA.,$,$,1.);"
                               : "IFCRECTANGLEPROFILEDEF(.AREA.,$,$,2.,2.);") +
            "#20=IFCEXTRUDEDAREASOLID(#15,#12,$,6.);"
            "#21=IFCCARTESIANPOINTLIST2D(((0.,0.),(4.,0.))); "
            "#22=IFCINDEXEDPOLYCURVE(#21,$,$);"
            "#50=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#20,#22));"
            "#51=IFCPRODUCTDEFINITIONSHAPE($,$,(#50)); "
            "#52=IFCOPENINGELEMENT('opening',$,$,$,$,$,#51,$);"
            "#53=IFCRELVOIDSELEMENT('void',$,$,$,#92,#52);",
        "#10,#22"));
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(productVolume(model), circle ? 64 - 4 * std::acos(-1.0) : 48,
                .09);
    EXPECT_EQ(
        std::ranges::count(model.elements,
                           container::geometry::dotbim::GeometryKind::Curves,
                           &container::geometry::dotbim::Element::geometryKind),
        1);
    ASSERT_EQ(model.nativeCurveRanges.size(), 1u);
    EXPECT_EQ(model.nativeCurveRanges[0].indexCount, 2u);
  }
}

TEST(PolylineGeometry, IndependentPathsAndRejectedInputsAreAtomic) {
  container::geometry::dotbim::Model model;
  const std::array<glm::vec3, 4> points{glm::vec3(0), glm::vec3(1, 0, 0),
                                        glm::vec3(3, 0, 0), glm::vec3(4, 0, 0)};
  const std::array<size_t, 2> counts{2, 2};
  ASSERT_TRUE(container::geometry::appendPolylineGeometry(model, points, counts,
                                                          7, .01f));
  ASSERT_EQ(model.nativeCurveRanges.size(), 1u);
  EXPECT_EQ(model.nativeCurveRanges[0].indexCount, 4u);
  const auto beforeVertices = model.vertices.size(),
             beforeIndices = model.indices.size();
  const std::array<size_t, 2> invalid{2, 3};
  EXPECT_FALSE(container::geometry::appendPolylineGeometry(model, points,
                                                           invalid, 8, .01f));
  auto nonfinite = points;
  nonfinite[3].x = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(container::geometry::appendPolylineGeometry(model, nonfinite,
                                                           counts, 8, .01f));
  EXPECT_EQ(model.vertices.size(), beforeVertices);
  EXPECT_EQ(model.indices.size(), beforeIndices);
  EXPECT_EQ(model.nativeCurveRanges.size(), 1u);
}

TEST(IfcTessellatedLoader, BooleanOperatorsProduceExpectedSolidVolumes) {
  for (const auto &[operation, volume] :
       std::array<std::pair<std::string, double>, 3>{
           {{"UNION", 96}, {"DIFFERENCE", 32}, {"INTERSECTION", 32}}}) {
    const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
        "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.);"
        "#10=IFCEXTRUDEDAREASOLID(#5,$,$,4.);"
        "#11=IFCCARTESIANPOINT((2.,0.,0.)); #12=IFCAXIS2PLACEMENT3D(#11,$,$);"
        "#13=IFCEXTRUDEDAREASOLID(#5,#12,$,4.); #20=IFCBOOLEANRESULT(." +
            operation + ".,#10,#13);",
        "#20"));
    ASSERT_FALSE(model.elements.empty()) << operation;
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(productVolume(model), volume, 1e-5) << operation;
    for (const auto &element : model.elements)
      EXPECT_EQ(element.guid, "solid-guid");
  }
}

TEST(IfcTessellatedLoader, HalfSpaceClippingHonorsAgreementAndOperation) {
  for (bool agreement : {false, true})
    for (bool difference : {false, true}) {
      const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
          "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.);"
          "#10=IFCEXTRUDEDAREASOLID(#5,$,$,4.);"
          "#11=IFCCARTESIANPOINT((0.,0.,1.)); #12=IFCAXIS2PLACEMENT3D(#11,$,$);"
          "#13=IFCPLANE(#12); #14=IFCHALFSPACESOLID(#13,." +
              std::string(agreement ? "T" : "F") + ".); #20=" +
              (difference ? "IFCBOOLEANCLIPPINGRESULT(.DIFFERENCE."
                          : "IFCBOOLEANRESULT(.INTERSECTION.") +
              ",#10,#14);",
          "#20"));
      ASSERT_FALSE(model.elements.empty());
      EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
      EXPECT_NEAR(productVolume(model), agreement == difference ? 48 : 16,
                  1e-5);
    }
}

TEST(IfcTessellatedLoader, NestedBooleansRetainResultStylesAndReportCycles) {
  const std::string base =
      "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.);"
      "#10=IFCEXTRUDEDAREASOLID(#5,$,$,4.);"
      "#11=IFCCARTESIANPOINT((2.,0.,0.)); #12=IFCAXIS2PLACEMENT3D(#11,$,$);"
      "#13=IFCEXTRUDEDAREASOLID(#5,#12,$,4.);"
      "#20=IFCBOOLEANRESULT(.UNION.,#10,#13);";
  const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
      base + "#21=IFCBOOLEANRESULT(.DIFFERENCE.,#20,#13);"
             "#30=IFCCOLOURRGB($,1.,0.,0.); "
             "#31=IFCSURFACESTYLERENDERING(#30,0.,$,$,$,$,$,$,.NOTDEFINED.);"
             "#32=IFCSURFACESTYLE($,.BOTH.,(#31)); "
             "#33=IFCPRESENTATIONSTYLEASSIGNMENT((#32));"
             "#34=IFCSTYLEDITEM(#21,(#33),$);",
      "#21"));
  ASSERT_FALSE(model.elements.empty());
  EXPECT_NEAR(productVolume(model), 32, 1e-5);
  for (const auto &element : model.elements)
    EXPECT_EQ(element.color, glm::vec4(1, 0, 0, 1));
  const auto cyclic = container::geometry::ifc::LoadFromStep(
      solidFixture(base + "#21=IFCBOOLEANRESULT(.DIFFERENCE.,#22,#10); "
                          "#22=IFCBOOLEANRESULT(.UNION.,#21,#13);",
                   "#21"));
  EXPECT_TRUE(cyclic.elements.empty());
  EXPECT_EQ(cyclic.importReport.completeness, ImportCompleteness::Failed);
  ASSERT_EQ(cyclic.importReport.diagnostics.size(), 1u);
  EXPECT_NE(cyclic.importReport.diagnostics[0].reason.find("Cyclic"),
            std::string::npos);
}

TEST(IfcTessellatedLoader, IntermediateBooleanStyleColorsClippingFaces) {
  const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
      "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.);"
      "#10=IFCEXTRUDEDAREASOLID(#5,$,$,4.);"
      "#20=IFCBOOLEANRESULT(.UNION.,#10,#10);"
      "#11=IFCCARTESIANPOINT((0.,0.,1.)); #12=IFCAXIS2PLACEMENT3D(#11,$,$);"
      "#13=IFCPLANE(#12); #14=IFCHALFSPACESOLID(#13,.T.);"
      "#21=IFCBOOLEANCLIPPINGRESULT(.DIFFERENCE.,#20,#14);"
      "#30=IFCCOLOURRGB($,1.,0.,0.);"
      "#31=IFCSURFACESTYLERENDERING(#30,0.,$,$,$,$,$,$,.NOTDEFINED.);"
      "#32=IFCSURFACESTYLE($,.BOTH.,(#31));"
      "#33=IFCPRESENTATIONSTYLEASSIGNMENT((#32));"
      "#34=IFCSTYLEDITEM(#20,(#33),$);",
      "#21"));
  ASSERT_FALSE(model.elements.empty());
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_NEAR(productVolume(model), 48, 1e-5);
  for (const auto &element : model.elements)
    EXPECT_EQ(element.color, glm::vec4(1, 0, 0, 1));
}

TEST(IfcTessellatedLoader,
     CircularAndHollowProfilesPreserveVolumeAndPlacement) {
  for (bool hollow : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
        "#2=IFCCARTESIANPOINT((5.,7.)); #3=IFCDIRECTION((0.,1.));"
        "#4=IFCAXIS2PLACEMENT2D(#2,#3); #5=" +
        std::string(hollow
                        ? "IFCCIRCLEHOLLOWPROFILEDEF(.AREA.,'Tube',#4,2.,0.5);"
                        : "IFCCIRCLEPROFILEDEF(.AREA.,'Circle',#4,2.);") +
        "#10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);"));
    ASSERT_EQ(model.elements.size(), 1u);
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    const double area = std::acos(-1.0) * (hollow ? 4.0 - 2.25 : 4.0);
    EXPECT_NEAR(signedMeshVolume(model), area * 3, area * 3 * .008);
    glm::vec3 min(std::numeric_limits<float>::max()), max(-min);
    for (const auto &v : model.vertices) {
      min = glm::min(min, v.position);
      max = glm::max(max, v.position);
    }
    EXPECT_NEAR(min.x, 3, 1e-6);
    EXPECT_NEAR(max.x, 7, 1e-6);
    EXPECT_NEAR(min.y, 5, 1e-6);
    EXPECT_NEAR(max.y, 9, 1e-6);
    EXPECT_NEAR(min.z, 0, 1e-6);
    EXPECT_NEAR(max.z, 3, 1e-6);
  }
}

TEST(IfcTessellatedLoader, SmallCircularProfilesHaveCorrectCapNormals) {
  const auto model = container::geometry::ifc::LoadFromStep(
      solidFixture("#5=IFCCIRCLEPROFILEDEF(.AREA.,$,$,0.005);"
                   "#10=IFCEXTRUDEDAREASOLID(#5,$,$,0.1);"));
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(signedMeshVolume(model), std::acos(-1.0) * .005 * .005 * .1,
              6e-8);
  for (size_t i = 0; i < model.indices.size(); i += 3) {
    const auto &a = model.vertices[model.indices[i]],
               &b = model.vertices[model.indices[i + 1]],
               &c = model.vertices[model.indices[i + 2]];
    if (a.position.z == b.position.z && b.position.z == c.position.z)
      EXPECT_NEAR(a.normal.z, a.position.z == 0 ? -1 : 1, 1e-6);
  }
}

TEST(IfcTessellatedLoader, SweptDisksUseTrimmedCurveLocalParameters) {
  for (bool reverse : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
        "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCDIRECTION((1.,0.,0.));"
        "#4=IFCVECTOR(#3,2.); #5=IFCLINE(#2,#4); #6=IFCTRIMMEDCURVE(#5," +
        std::string(
            reverse ? "(IFCPARAMETERVALUE(7.)),(IFCPARAMETERVALUE(3.)),.F."
                    : "(IFCPARAMETERVALUE(3.)),(IFCPARAMETERVALUE(7.)),.T.") +
        ",.PARAMETER.); #10=IFCSWEPTDISKSOLID(#6,1.,0.5,1.,3.);"));
    ASSERT_EQ(model.elements.size(), 1u);
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(signedMeshVolume(model), std::acos(-1.0) * .75 * 4, .08);
    float min = std::numeric_limits<float>::max(), max = -min;
    for (const auto &v : model.vertices) {
      min = std::min(min, v.position.x);
      max = std::max(max, v.position.x);
    }
    EXPECT_NEAR(min, 8, 1e-6);
    EXPECT_NEAR(max, 12, 1e-6);
  }
}

TEST(IfcTessellatedLoader, SweptDisksAcceptCartesianTrimsAndBoundedDefaults) {
  const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCDIRECTION((0.,0.,1.));"
      "#4=IFCVECTOR(#3,2.); #5=IFCLINE(#2,#4);"
      "#7=IFCCARTESIANPOINT((0.,0.,6.)); #8=IFCCARTESIANPOINT((0.,0.,10.));"
      "#6=IFCTRIMMEDCURVE(#5,(#7),(#8),.T.,.CARTESIAN.);"
      "#10=IFCSWEPTDISKSOLID(#6,1.,$,$,$);"));
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(signedMeshVolume(model), std::acos(-1.0) * 4, .09);
  for (const auto &v : model.vertices) {
    EXPECT_GE(v.position.z, 6);
    EXPECT_LE(v.position.z, 10);
  }
}

TEST(IfcTessellatedLoader, RejectsInvalidCircularProfilesAndSweepDomains) {
  const std::array<std::string, 6> bodies{
      "#5=IFCCIRCLEHOLLOWPROFILEDEF(.AREA.,$,$,1.,1.); "
      "#10=IFCEXTRUDEDAREASOLID(#5,$,$,2.);",
      "#5=IFCCIRCLEPROFILEDEF(.AREA.,$,$,-1.); "
      "#10=IFCEXTRUDEDAREASOLID(#5,$,$,2.);",
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCDIRECTION((1.,0.,0.)); "
      "#4=IFCVECTOR(#3,1.); #5=IFCLINE(#2,#4); "
      "#10=IFCSWEPTDISKSOLID(#5,1.,$,$,$);",
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCDIRECTION((1.,0.,0.)); "
      "#4=IFCVECTOR(#3,1.); #5=IFCLINE(#2,#4); "
      "#6=IFCTRIMMEDCURVE(#5,(IFCPARAMETERVALUE(2.)),(IFCPARAMETERVALUE(4.)),."
      "T.,.PARAMETER.); #10=IFCSWEPTDISKSOLID(#6,1.,$,0.,3.);",
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCDIRECTION((1.,0.,0.)); "
      "#4=IFCVECTOR(#3,1.); #5=IFCLINE(#2,#4); "
      "#6=IFCTRIMMEDCURVE(#5,(IFCPARAMETERVALUE(4.)),(IFCPARAMETERVALUE(2.)),."
      "T.,.PARAMETER.); #10=IFCSWEPTDISKSOLID(#6,1.,$,$,$);",
      "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCDIRECTION((1.,0.,0.)); "
      "#4=IFCVECTOR(#3,1.); #5=IFCLINE(#2,#4); "
      "#7=IFCCARTESIANPOINT((2.,1.,0.)); #8=IFCCARTESIANPOINT((4.,0.,0.)); "
      "#6=IFCTRIMMEDCURVE(#5,(#7),(#8),.T.,.CARTESIAN.); "
      "#10=IFCSWEPTDISKSOLID(#6,1.,$,$,$);"};
  for (const auto &body : bodies) {
    const auto model =
        container::geometry::ifc::LoadFromStep(solidFixture(body));
    EXPECT_TRUE(model.elements.empty());
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_EQ(model.importReport.skippedProductCount, 1u);
  }
}

TEST(IfcTessellatedLoader, StructuralProfilesIncludeFilletsAndEdgeRadii) {
  const double cornerArea = 1 - std::acos(-1.0) / 4;
  for (const auto &[profile, area] :
       std::array<std::pair<std::string, double>, 3>{
           {{"IFCLSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,0.5,0.2,$)",
             13 + cornerArea * (.25 - 2 * .04)},
            {"IFCUSHAPEPROFILEDEF(.AREA.,$,$,8.,6.,1.,1.,0.5,$,$)",
             18 + cornerArea * 2 * .25},
            {"IFCISHAPEPROFILEDEF(.AREA.,$,$,6.,8.,1.,1.,0.5)",
             18 + cornerArea * 4 * .25}}}) {
    const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
        "#5=" + profile + "; #10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);"));
    ASSERT_EQ(model.elements.size(), 1u) << profile;
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(signedMeshVolume(model), area * 3, area * 3 * .0002) << profile;
  }
}

TEST(IfcTessellatedLoader, CircularSweepsMatchAnalyticTubeVolumeAndNormals) {
  for (bool degrees : {false, true}) {
    const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
        "#2=IFCCARTESIANPOINT((0.,0.,0.)); #3=IFCAXIS2PLACEMENT3D(#2,$,$);"
        "#4=IFCCIRCLE(#3,5.); "
        "#6=IFCTRIMMEDCURVE(#4,(IFCPARAMETERVALUE(0.)),(IFCPARAMETERVALUE(" +
        std::string(degrees ? "90." : "1.5707963267948966") +
        ")),.T.,.PARAMETER.);"
        "#10=IFCSWEPTDISKSOLID(#6,2.,1.,$,$);" +
        (degrees ? std::string("#70=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.); "
                               "#71=IFCMEASUREWITHUNIT(IFCPLANEANGLEMEASURE(0."
                               "017453292519943295),#70); "
                               "#72=IFCCONVERSIONBASEDUNIT($,.PLANEANGLEUNIT.,'"
                               "Degree',#71); #73=IFCUNITASSIGNMENT((#1,#72)); "
                               "#74=IFCPROJECT('project',$,$,$,$,$,$,(),#73);")
                 : "")));
    ASSERT_EQ(model.elements.size(), 1u);
    const double volume = std::acos(-1.0) * 3 * 5 * std::acos(-1.0) / 2;
    EXPECT_NEAR(signedMeshVolume(model), volume, volume * .009);
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    for (size_t i = 0; i < model.indices.size(); i += 3) {
      const auto &a = model.vertices[model.indices[i]],
                 &b = model.vertices[model.indices[i + 1]],
                 &c = model.vertices[model.indices[i + 2]];
      if (a.position.y == 0 && b.position.y == 0 && c.position.y == 0)
        EXPECT_NEAR(a.normal.y, -1, 1e-5);
    }
  }
}

TEST(IfcTessellatedLoader, CompositeSweepsAccumulateDomainsAndReverseSegments) {
  const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
      "#2=IFCCARTESIANPOINT((0.,-3.,0.)); #3=IFCDIRECTION((0.,1.,0.)); "
      "#4=IFCVECTOR(#3,1.); #5=IFCLINE(#2,#4);"
      "#6=IFCTRIMMEDCURVE(#5,(IFCPARAMETERVALUE(0.)),(IFCPARAMETERVALUE(3.)),."
      "T.,.PARAMETER.);"
      "#11=IFCCARTESIANPOINT((-5.,0.,0.)); #12=IFCAXIS2PLACEMENT3D(#11,$,$); "
      "#13=IFCCIRCLE(#12,5.);"
      "#14=IFCTRIMMEDCURVE(#13,(IFCPARAMETERVALUE(0.)),(IFCPARAMETERVALUE(1."
      "5707963267948966)),.T.,.PARAMETER.);"
      "#15=IFCCARTESIANPOINT((-8.,5.,0.)); #16=IFCDIRECTION((1.,0.,0.)); "
      "#17=IFCVECTOR(#16,1.); #18=IFCLINE(#15,#17);"
      "#19=IFCTRIMMEDCURVE(#18,(IFCPARAMETERVALUE(0.)),(IFCPARAMETERVALUE(3.)),"
      ".T.,.PARAMETER.);"
      "#20=IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#6); "
      "#21=IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#14);"
      "#22=IFCCOMPOSITECURVESEGMENT(.DISCONTINUOUS.,.F.,#19); "
      "#23=IFCCOMPOSITECURVE((#20,#21,#22),.F.);"
      "#10=IFCSWEPTDISKSOLID(#23,1.,$,1.,6.5707963267948966);"));
  ASSERT_EQ(model.elements.size(), 1u);
  const double volume = std::acos(-1.0) * (4 + 5 * std::acos(-1.0) / 2);
  EXPECT_NEAR(signedMeshVolume(model), volume, volume * .009);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
}

TEST(IfcTessellatedLoader, CurvedProfileAndDuplicatePolylineEdgesRemainValid) {
  const auto curved = container::geometry::ifc::LoadFromStep(
      solidFixture("#2=IFCCARTESIANPOINT((0.,0.)); "
                   "#3=IFCAXIS2PLACEMENT2D(#2,$); #4=IFCCIRCLE(#3,2.);"
                   "#6=IFCTRIMMEDCURVE(#4,(IFCPARAMETERVALUE(0.)),("
                   "IFCPARAMETERVALUE(3.141592653589793)),.T.,.PARAMETER.);"
                   "#7=IFCCARTESIANPOINT((-2.,0.)); "
                   "#8=IFCCARTESIANPOINT((2.,0.)); #9=IFCPOLYLINE((#7,#8));"
                   "#11=IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#6); "
                   "#12=IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,#9);"
                   "#13=IFCCOMPOSITECURVE((#11,#12),.U.); "
                   "#5=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#13);"
                   "#10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);"));
  ASSERT_EQ(curved.elements.size(), 1u);
  EXPECT_NEAR(signedMeshVolume(curved), std::acos(-1.0) * 2 * 3, .04);
  const auto duplicate = container::geometry::ifc::LoadFromStep(solidFixture(
      "#2=IFCCARTESIANPOINT((0.,0.)); #3=IFCCARTESIANPOINT((2.,0.));"
      "#4=IFCCARTESIANPOINT((2.,2.)); #6=IFCCARTESIANPOINT((0.,2.));"
      "#7=IFCPOLYLINE((#2,#3,#3,#4,#6,#2)); "
      "#5=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#7);"
      "#10=IFCEXTRUDEDAREASOLID(#5,$,$,3.);"));
  ASSERT_EQ(duplicate.elements.size(), 1u);
  EXPECT_NEAR(signedMeshVolume(duplicate), 12, 1e-6);
}

TEST(IfcTessellatedLoader,
     KernelOpeningsHandleCircularRotatedAndOverlappingCuts) {
  const std::string host =
      "#5=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,4.,4.); "
      "#10=IFCEXTRUDEDAREASOLID(#5,$,$,4.);"
      "#11=IFCCARTESIANPOINT((0.,0.,-1.)); #12=IFCAXIS2PLACEMENT3D(#11,$,$);";
  const std::string relation =
      "#50=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#20));"
      "#51=IFCPRODUCTDEFINITIONSHAPE($,$,(#50)); "
      "#52=IFCOPENINGELEMENT('opening',$,$,$,$,$,#51,$);"
      "#53=IFCRELVOIDSELEMENT('void',$,$,$,#92,#52);";
  for (const auto &[profile, volume] :
       std::array<std::pair<std::string, double>, 2>{
           {{"#15=IFCCIRCLEPROFILEDEF(.AREA.,$,$,1.);",
             64 - 4 * std::acos(-1.0)},
            {"#16=IFCCARTESIANPOINT((0.,0.)); #17=IFCDIRECTION((1.,1.)); "
             "#18=IFCAXIS2PLACEMENT2D(#16,#17); "
             "#15=IFCRECTANGLEPROFILEDEF(.AREA.,$,#18,2.,1.);",
             56}}}) {
    const auto model = container::geometry::ifc::LoadFromStep(solidFixture(
        host + profile + "#20=IFCEXTRUDEDAREASOLID(#15,#12,$,6.);" + relation));
    ASSERT_FALSE(model.elements.empty());
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
    EXPECT_NEAR(productVolume(model), volume, .09);
    for (const auto &element : model.elements)
      EXPECT_EQ(element.guid, "solid-guid");
  }
  const auto overlap = container::geometry::ifc::LoadFromStep(solidFixture(
      host +
      "#15=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,2.,2.); "
      "#20=IFCEXTRUDEDAREASOLID(#15,#12,$,6.);" +
      relation +
      "#60=IFCCARTESIANPOINT((0.5,0.,-1.)); #61=IFCAXIS2PLACEMENT3D(#60,$,$); "
      "#62=IFCEXTRUDEDAREASOLID(#15,#61,$,6.);"
      "#63=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#62)); "
      "#64=IFCPRODUCTDEFINITIONSHAPE($,$,(#63));"
      "#65=IFCOPENINGELEMENT('opening2',$,$,$,$,$,#64,$); "
      "#66=IFCRELVOIDSELEMENT('void2',$,$,$,#92,#65);"));
  ASSERT_FALSE(overlap.elements.empty());
  EXPECT_EQ(overlap.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_NEAR(productVolume(overlap), 44, 1e-5);
}

TEST(IfcTessellatedLoader, TriangulatesConcavePolygonWithAuthoredNormals) {
  const auto model = container::geometry::ifc::LoadFromStep(polygonFixture(
      "((0.,0.,0.),(2.,0.,0.),(2.,1.,0.),(1.,1.,0.),(1.,2.,0.),(0.,2.,0.))",
      "IFCINDEXEDPOLYGONALFACE((1,2,3,4,5,6))"));
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(meshArea(model), 3.0, 1e-6);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  EXPECT_EQ(model.importReport.sourceProductCount, 1u);
  EXPECT_EQ(model.importReport.importedProductCount, 1u);
  for (const auto &vertex : model.vertices) {
    EXPECT_NEAR(vertex.normal.z, 1.0f, 1e-6f);
    const auto worldNormal =
        glm::mat3(model.elements.front().transform) * vertex.normal;
    EXPECT_NEAR(worldNormal.y, 1.0f, 1e-6f);
  }
}

TEST(IfcTessellatedLoader, PreservesPolygonHolesAndPnIndexOnVerticalPlane) {
  // First coordinate is deliberately unused. PnIndex shifts every face index.
  const auto model = container::geometry::ifc::LoadFromStep(polygonFixture(
      "((999.,999.,999.),(0.,5.,0.),(4.,5.,0.),(4.,5.,4.),(0.,5.,4.),"
      "(1.,5.,1.),(1.,5.,3.),(3.,5.,3.),(3.,5.,1.))",
      "IFCINDEXEDPOLYGONALFACEWITHVOIDS((1,2,3,4),((5,6,7,8)))",
      "(2,3,4,5,6,7,8,9)"));
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(meshArea(model), 12.0, 1e-6);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  for (const auto &vertex : model.vertices) {
    EXPECT_NEAR(vertex.position.y, 5.0f, 1e-6f);
    EXPECT_NEAR(vertex.normal.y, -1.0f, 1e-6f);
  }
  for (size_t i = 0; i < model.indices.size(); i += 3u) {
    const auto centroid = (model.vertices[model.indices[i]].position +
                           model.vertices[model.indices[i + 1u]].position +
                           model.vertices[model.indices[i + 2u]].position) /
                          3.0f;
    EXPECT_FALSE(centroid.x > 1 && centroid.x < 3 && centroid.z > 1 &&
                 centroid.z < 3);
  }
}

TEST(IfcTessellatedLoader, RejectsMalformedPolygonTopologyAndIndices) {
  const std::array<std::string, 4> fixtures{
      polygonFixture("((0.,0.,0.),(4.,0.,0.),(4.,4.,0.),(0.,4.,0.),"
                     "(5.,1.,0.),(5.,2.,0.),(6.,2.,0.),(6.,1.,0.))",
                     "IFCINDEXEDPOLYGONALFACEWITHVOIDS((1,2,3,4),((5,6,7,8)))"),
      polygonFixture("((0.,0.,0.),(2.,2.,0.),(0.,2.,0.),(2.,0.,0.))",
                     "IFCINDEXEDPOLYGONALFACE((1,2,3,4))"),
      polygonFixture("((0.,0.,0.),(1.,0.,0.),(0.,1.,0.))",
                     "IFCINDEXEDPOLYGONALFACE((1,2,3))", "(1,$,3)"),
      polygonFixture("((0.,0.,0.),(1.,$ ,0.),(1.,0.,0.),(0.,1.,0.))",
                     "IFCINDEXEDPOLYGONALFACE((1,2,3))")};
  for (const auto &fixture : fixtures) {
    const auto model = container::geometry::ifc::LoadFromStep(fixture);
    EXPECT_TRUE(model.elements.empty());
    EXPECT_TRUE(model.vertices.empty());
    EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
    EXPECT_EQ(model.importReport.skippedProductCount, 1u);
    ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
    EXPECT_EQ(model.importReport.diagnostics.front().entityId, 21u);
    EXPECT_EQ(model.importReport.diagnostics.front().productGuid,
              "polygon-guid");
  }
}

TEST(IfcTessellatedLoader, ReportsUnsupportedMappedProductsWithoutRawFallback) {
  constexpr auto step = R"ifc(ISO-10303-21; DATA;
    #20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)));
    #21=IFCTRIANGULATEDFACESET(#20,$,.F.,((1,2,3)),$);
    #22=IFCBOOLEANRESULT(.DIFFERENCE.,#21,#21);
    #30=IFCSHAPEREPRESENTATION($,'Body','CSG',(#22));
    #31=IFCREPRESENTATIONMAP($,#30);
    #32=IFCMAPPEDITEM(#31,$);
    #33=IFCSHAPEREPRESENTATION($,'Body','MappedRepresentation',(#32));
    #34=IFCPRODUCTDEFINITIONSHAPE($,$,(#33));
    #40=IFCBUILDINGELEMENTPROXY('missing-guid',$,$,$,$,$,#34,$,$);
    ENDSEC; END-ISO-10303-21;)ifc";
  const auto model = container::geometry::ifc::LoadFromStep(step);
  EXPECT_TRUE(
      model.elements.empty()); // Operand meshes must not become ghost products.
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Failed);
  EXPECT_EQ(model.importReport.sourceProductCount, 1u);
  EXPECT_EQ(model.importReport.skippedProductCount, 1u);
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  const auto &diagnostic = model.importReport.diagnostics.front();
  EXPECT_EQ(diagnostic.entityId, 22u);
  EXPECT_EQ(diagnostic.productId, 40u);
  EXPECT_EQ(diagnostic.productGuid, "missing-guid");
  EXPECT_EQ(diagnostic.representationType, "IFCBOOLEANRESULT");
}

TEST(IfcTessellatedLoader,
     CountsProductsRatherThanColorGroupsAndDeduplicatesWarnings) {
  const auto model = container::geometry::ifc::LoadFromStep(polygonFixture(
      "((0.,0.,0.),(1.,0.,0.),(0.,1.,0.))", "IFCINDEXEDPOLYGONALFACE((1,2,3))",
      "$",
      "#41=IFCSWEPTDISKSOLID($,1.,$,$,$);"
      "#42=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#21,#41,#41));"
      "#43=IFCPRODUCTDEFINITIONSHAPE($,$,(#42));"
      "#44=IFCBUILDINGELEMENTPROXY('partial-guid',$,$,$,$,$,#43,$,$);"
      "#45=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#41));"
      "#46=IFCPRODUCTDEFINITIONSHAPE($,$,(#45));"
      "#47=IFCBUILDINGELEMENTPROXY('skipped-guid',$,$,$,$,$,#46,$,$);"));
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Partial);
  EXPECT_EQ(model.importReport.sourceProductCount, 3u);
  EXPECT_EQ(model.importReport.importedProductCount, 2u);
  EXPECT_EQ(model.importReport.skippedProductCount, 1u);
  EXPECT_EQ(model.importReport.partialProductCount, 1u);
  ASSERT_EQ(model.importReport.diagnostics.size(), 2u);
  EXPECT_EQ(model.importReport.representationWarnings.at("IFCSWEPTDISKSOLID"),
            2u);
}

TEST(IfcTessellatedLoader,
     TriangulatedPnIndexAndInvalidFacesAreHandledAtomically) {
  const std::string prefix = "ISO-10303-21; DATA;"
                             "#20=IFCCARTESIANPOINTLIST3D(((999.,999.,999.),(0."
                             ",0.,0.),(2.,0.,0.),(0.,2.,0.)));";
  const std::string suffix =
      "#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));"
      "#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));"
      "#40=IFCBUILDINGELEMENTPROXY('triangle-guid',$,$,$,$,$,#31,$,$); ENDSEC; "
      "END-ISO-10303-21;";
  const auto valid = container::geometry::ifc::LoadFromStep(
      prefix + "#21=IFCTRIANGULATEDFACESET(#20,$,.F.,((1,2,3)),(2,3,4));" +
      suffix);
  ASSERT_EQ(valid.elements.size(), 1u);
  EXPECT_NEAR(meshArea(valid), 2.0, 1e-6);
  const auto invalid = container::geometry::ifc::LoadFromStep(
      prefix + "#21=IFCTRIANGULATEDFACESET(#20,$,.F.,((2,3,4),(2,3,99)),$);" +
      suffix);
  EXPECT_TRUE(invalid.vertices.empty());
  EXPECT_EQ(invalid.importReport.completeness, ImportCompleteness::Failed);
}

TEST(IfcTessellatedLoader,
     FacetedBrepCubeHasCorrectAreaVolumeAndOutwardNormals) {
  std::string step = "ISO-10303-21; DATA;";
  const std::array<std::array<int, 3>, 8> points{{{-1, -1, -1},
                                                  {1, -1, -1},
                                                  {1, 1, -1},
                                                  {-1, 1, -1},
                                                  {-1, -1, 1},
                                                  {1, -1, 1},
                                                  {1, 1, 1},
                                                  {-1, 1, 1}}};
  for (size_t i = 0; i < points.size(); ++i)
    step += "#" + std::to_string(i + 1u) + "=IFCCARTESIANPOINT((" +
            std::to_string(points[i][0]) + "," + std::to_string(points[i][1]) +
            "," + std::to_string(points[i][2]) + "));";
  const std::array<std::array<int, 4>, 6> faces{{{1, 4, 3, 2},
                                                 {5, 6, 7, 8},
                                                 {1, 2, 6, 5},
                                                 {2, 3, 7, 6},
                                                 {3, 4, 8, 7},
                                                 {4, 1, 5, 8}}};
  std::string faceRefs;
  for (size_t i = 0; i < faces.size(); ++i) {
    const auto id = 100u + i * 3u;
    std::string refs;
    // Reverse one polyloop and use Orientation=.F. to recover its outward face.
    for (size_t j = 0; j < 4u; ++j) {
      if (j)
        refs += ",";
      refs += "#" + std::to_string(faces[i][i == 0 ? 3u - j : j]);
    }
    step += "#" + std::to_string(id) + "=IFCPOLYLOOP((" + refs + "));";
    step += "#" + std::to_string(id + 1u) + "=IFCFACEOUTERBOUND(#" +
            std::to_string(id) + (i == 0 ? ",.F.);" : ",.T.);");
    step += "#" + std::to_string(id + 2u) + "=IFCFACE((#" +
            std::to_string(id + 1u) + "));";
    if (i)
      faceRefs += ",";
    faceRefs += "#" + std::to_string(id + 2u);
  }
  step += "#200=IFCCLOSEDSHELL((" + faceRefs +
          ")); #201=IFCFACETEDBREP(#200);"
          "#202=IFCSHAPEREPRESENTATION($,'Body','Brep',(#201));"
          "#203=IFCPRODUCTDEFINITIONSHAPE($,$,(#202));"
          "#204=IFCBUILDINGELEMENTPROXY('brep-guid',$,$,$,$,$,#203,$,$); "
          "ENDSEC; END-ISO-10303-21;";
  const auto model = container::geometry::ifc::LoadFromStep(step);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(meshArea(model), 24.0, 1e-6);
  EXPECT_NEAR(signedMeshVolume(model), 8.0, 1e-6);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  for (const auto &vertex : model.vertices)
    EXPECT_GT(glm::dot(vertex.position, vertex.normal), 0.99f);
  const auto end = step.find(" ENDSEC;");
  ASSERT_NE(end, std::string::npos);
  step.insert(
      end,
      " #300=IFCCOLOURRGB($,1.,0.,0.); #301=IFCSURFACESTYLESHADING(#300,0.);"
      "#302=IFCSURFACESTYLE('Red face',.BOTH.,(#301)); "
      "#303=IFCSTYLEDITEM(#102,(#302),$);");
  const auto colored = container::geometry::ifc::LoadFromStep(step);
  ASSERT_EQ(colored.elements.size(), 2u);
  EXPECT_EQ(colored.importReport.importedProductCount, 1u);
  EXPECT_NEAR(meshArea(colored), 24.0, 1e-6);
  EXPECT_NEAR(signedMeshVolume(colored), 8.0, 1e-6);
  EXPECT_TRUE(std::ranges::any_of(colored.elements, [](const auto &e) {
    return e.color.r > .99f && e.color.g < .01f;
  }));
}

TEST(IfcTessellatedLoader,
     ExtrudedProfileWithVoidHasCorrectVolumeAndInnerWalls) {
  constexpr auto step = R"ifc(ISO-10303-21; DATA;
    #1=IFCCARTESIANPOINT((0.,0.)); #2=IFCCARTESIANPOINT((4.,0.));
    #3=IFCCARTESIANPOINT((4.,4.)); #4=IFCCARTESIANPOINT((0.,4.));
    #5=IFCCARTESIANPOINT((1.,1.)); #6=IFCCARTESIANPOINT((3.,1.));
    #7=IFCCARTESIANPOINT((3.,3.)); #8=IFCCARTESIANPOINT((1.,3.));
    #10=IFCPOLYLINE((#4,#3,#2,#1,#4)); #11=IFCPOLYLINE((#5,#6,#7,#8,#5));
    #12=IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,$,#10,(#11));
    #13=IFCDIRECTION((0.,0.,1.)); #14=IFCEXTRUDEDAREASOLID(#12,$,#13,2.);
    #20=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#14));
    #21=IFCPRODUCTDEFINITIONSHAPE($,$,(#20));
    #22=IFCBUILDINGELEMENTPROXY('extruded-guid',$,$,$,$,$,#21,$,$);
    ENDSEC; END-ISO-10303-21;)ifc";
  const auto model = container::geometry::ifc::LoadFromStep(step);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(signedMeshVolume(model), 24.0, 1e-6); // (16 - 4) * 2
  EXPECT_NEAR(meshArea(model), 72.0,
              1e-6); // 2 caps + outer/inner perimeter * height
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
  auto reversed = std::string(step);
  const auto direction = reversed.find("IFCDIRECTION((0.,0.,1.))");
  ASSERT_NE(direction, std::string::npos);
  reversed.replace(direction, std::string("IFCDIRECTION((0.,0.,1.))").size(),
                   "IFCDIRECTION((0.,0.,-1.))");
  const auto negative = container::geometry::ifc::LoadFromStep(reversed);
  ASSERT_EQ(negative.elements.size(), 1u);
  EXPECT_NEAR(signedMeshVolume(negative), 24.0, 1e-6);
  EXPECT_NEAR(meshArea(negative), 72.0, 1e-6);
}

TEST(IfcTessellatedLoader,
     PreservesMappedPolygonMirrorsUnitsColorsAndIdentity) {
  auto step = polygonFixture(
      "((0.,0.,0.),(1000.,0.,0.),(0.,1000.,0.))",
      "IFCINDEXEDPOLYGONALFACE((1,2,3))", "$",
      "#50=IFCDIRECTION((-1.,0.,0.)); #51=IFCDIRECTION((0.,1.,0.)); "
      "#52=IFCDIRECTION((0.,0.,1.));"
      "#53=IFCCARTESIANPOINT((3000.,0.,0.));"
      "#54=IFCCARTESIANTRANSFORMATIONOPERATOR3D(#50,#51,#53,2.,#52);"
      "#55=IFCREPRESENTATIONMAP($,#30); #56=IFCMAPPEDITEM(#55,#54);"
      "#57=IFCSHAPEREPRESENTATION($,'Body','MappedRepresentation',(#56));"
      "#58=IFCPRODUCTDEFINITIONSHAPE($,$,(#57));"
      "#59=IFCCOLOURRGB($,1.,0.,0.); #60=IFCSURFACESTYLESHADING(#59,0.);"
      "#61=IFCSURFACESTYLE('Red',.BOTH.,(#60)); "
      "#62=IFCSTYLEDITEM(#21,(#61),$);");
  step.replace(step.find(".LENGTHUNIT.,$,.METRE."),
               std::string(".LENGTHUNIT.,$,.METRE.").size(),
               ".LENGTHUNIT.,.MILLI.,.METRE.");
  step.replace(step.find("'Polygon',$,$,$,#31"),
               std::string("'Polygon',$,$,$,#31").size(),
               "'Polygon',$,$,$,#58");
  const auto model = container::geometry::ifc::LoadFromStep(step);
  ASSERT_EQ(model.elements.size(), 1u);
  const auto &element = model.elements.front();
  EXPECT_EQ(element.guid, "polygon-guid");
  EXPECT_EQ(element.sourceId, "#40");
  EXPECT_NEAR(element.color.r, 1.0f, 1e-6);
  EXPECT_NEAR(element.color.g, 0.0f, 1e-6);
  EXPECT_LT(glm::determinant(glm::mat3(element.transform)), 0.0f);
  glm::vec3 minimum(1e10f), maximum(-1e10f);
  for (const auto &vertex : model.vertices) {
    const auto world =
        glm::vec3(element.transform * glm::vec4(vertex.position, 1));
    minimum = glm::min(minimum, world);
    maximum = glm::max(maximum, world);
  }
  EXPECT_NEAR(minimum.x, 1.0f, 1e-6);
  EXPECT_NEAR(maximum.x, 3.0f, 1e-6);
  EXPECT_NEAR(minimum.z, -2.0f, 1e-6);
  EXPECT_NEAR(maximum.z, 0.0f, 1e-6);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
}

TEST(IfcTessellatedLoader,
     IndexedPolylineWallWithTwoOpeningsHasCorrectVolumeAndArea) {
  constexpr auto step = R"ifc(ISO-10303-21; DATA;
    #1=IFCCARTESIANPOINTLIST2D(((0.,0.),(10.,0.),(10.,1.),(0.,1.),(0.,0.)));
    #2=IFCINDEXEDPOLYCURVE(#1,$,.F.); #3=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#2);
    #4=IFCDIRECTION((0.,0.,1.)); #5=IFCEXTRUDEDAREASOLID(#3,$,#4,3.);
    #6=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#5));
    #7=IFCPRODUCTDEFINITIONSHAPE($,$,(#6)); #8=IFCWALL('wall',$,$,$,$,$,#7,$,$);
    #10=IFCCARTESIANPOINTLIST2D(((0.,0.),(1.,0.),(1.,1.),(0.,1.)));
    #11=IFCINDEXEDPOLYCURVE(#10,(IFCLINEINDEX((1,2,3)),IFCLINEINDEX((3,4,1))),.F.);
    #12=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#11); #13=IFCEXTRUDEDAREASOLID(#12,$,#4,1.);
    #14=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#13));
    #15=IFCPRODUCTDEFINITIONSHAPE($,$,(#14));
    #20=IFCCARTESIANPOINT((2.,0.,1.)); #21=IFCAXIS2PLACEMENT3D(#20,$,$);
    #22=IFCLOCALPLACEMENT($,#21); #23=IFCOPENINGELEMENT('opening1',$,$,$,$,#22,#15,$);
    #24=IFCCARTESIANPOINT((6.,0.,1.)); #25=IFCAXIS2PLACEMENT3D(#24,$,$);
    #26=IFCLOCALPLACEMENT($,#25); #27=IFCOPENINGELEMENT('opening2',$,$,$,$,#26,#15,$);
    #30=IFCRELVOIDSELEMENT('void1',$,$,$,#8,#23); #31=IFCRELVOIDSELEMENT('void2',$,$,$,#8,#27);
    ENDSEC; END-ISO-10303-21;)ifc";
  const auto model = container::geometry::ifc::LoadFromStep(step);
  ASSERT_EQ(model.elements.size(), 1u);
  const auto &element = model.elements.front();
  const auto range = std::ranges::find_if(model.meshRanges, [&](const auto &r) {
    return r.meshId == element.meshId;
  });
  ASSERT_NE(range, model.meshRanges.end());
  IfcModel drawn;
  drawn.vertices = model.vertices;
  drawn.indices.assign(model.indices.begin() + range->firstIndex,
                       model.indices.begin() + range->firstIndex +
                           range->indexCount);
  EXPECT_NEAR(signedMeshVolume(drawn), 28.0, 1e-6);
  EXPECT_NEAR(meshArea(drawn), 90.0, 1e-6);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
}

TEST(IfcTessellatedLoader, RectangleProfileHonorsIts2DPlacement) {
  constexpr auto step = R"ifc(ISO-10303-21; DATA;
    #1=IFCCARTESIANPOINT((10.,20.)); #2=IFCDIRECTION((0.,1.));
    #3=IFCAXIS2PLACEMENT2D(#1,#2); #4=IFCRECTANGLEPROFILEDEF(.AREA.,'4x2',#3,4.,2.);
    #5=IFCDIRECTION((0.,0.,1.)); #6=IFCEXTRUDEDAREASOLID(#4,$,#5,3.);
    #7=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#6));
    #8=IFCPRODUCTDEFINITIONSHAPE($,$,(#7));
    #9=IFCCOLUMN('rectangle-guid',$,$,$,$,$,#8,$,$); ENDSEC; END-ISO-10303-21;)ifc";
  const auto model = container::geometry::ifc::LoadFromStep(step);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(meshArea(model), 52.0, 1e-6);
  EXPECT_NEAR(signedMeshVolume(model), 24.0, 1e-6);
  glm::vec3 minimum(1e10f), maximum(-1e10f);
  for (const auto &vertex : model.vertices) {
    minimum = glm::min(minimum, vertex.position);
    maximum = glm::max(maximum, vertex.position);
  }
  EXPECT_NEAR(minimum.x, 9.0f, 1e-6);
  EXPECT_NEAR(maximum.x, 11.0f, 1e-6);
  EXPECT_NEAR(minimum.y, 18.0f, 1e-6);
  EXPECT_NEAR(maximum.y, 22.0f, 1e-6);
  EXPECT_NEAR(minimum.z, 0.0f, 1e-6);
  EXPECT_NEAR(maximum.z, 3.0f, 1e-6);
  EXPECT_EQ(model.importReport.completeness, ImportCompleteness::Complete);
}

TEST(IfcTessellatedLoader, StandaloneColorGroupsDoNotInventProductCounts) {
  constexpr auto step = R"ifc(ISO-10303-21; DATA;
    #1=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.),(0.,0.,1.)));
    #2=IFCTRIANGULATEDFACESET(#1,$,.F.,((1,2,3),(1,4,2)),$);
    #3=IFCCOLOURRGBLIST(((1.,0.,0.),(0.,1.,0.)));
    #4=IFCINDEXEDCOLOURMAP(#2,$,#3,(1,2)); ENDSEC; END-ISO-10303-21;)ifc";
  const auto model = container::geometry::ifc::LoadFromStep(step);
  ASSERT_EQ(model.elements.size(), 2u);
  EXPECT_EQ(model.importReport.sourceProductCount, 0u);
  EXPECT_EQ(model.importReport.importedProductCount, 0u);
  EXPECT_NEAR(meshArea(model), 1.0, 1e-6);
}

TEST(IfcTessellatedLoader, BuildingSmartSamplesExposeImportCoverage) {
  const char *sampleRoot = std::getenv("CONTAINER_IFC_SAMPLE_ROOT");
  if (!sampleRoot)
    GTEST_SKIP() << "Set CONTAINER_IFC_SAMPLE_ROOT to the buildingSMART "
                    "examples directory";
  nlohmann::json results = nlohmann::json::array();
  for (const auto &relative :
       {"Hello Wall/hello-wall.ifc", "Tekla House/TeklaHouse.ifc"}) {
    const auto path = std::filesystem::path(sampleRoot) / relative;
    const auto model = container::geometry::ifc::LoadFromFile(path);
    ASSERT_FALSE(model.elements.empty()) << path;
    const auto &report = model.importReport;
    EXPECT_EQ(report.completeness, ImportCompleteness::Complete);
    EXPECT_EQ(report.partialProductCount, 0u);
    EXPECT_TRUE(report.diagnostics.empty());
    const bool hello = std::string_view(relative).starts_with("Hello");
    EXPECT_EQ(report.importedProductCount, hello ? 4u : 10042u);
    EXPECT_EQ(model.nativeCurveRanges.size(), hello ? 6u : 22u);
    const auto curveInstances = std::ranges::count(
        model.elements, container::geometry::dotbim::GeometryKind::Curves,
        &container::geometry::dotbim::Element::geometryKind);
    EXPECT_EQ(curveInstances, hello ? 11 : 43);
    EXPECT_EQ(report.sourceProductCount,
              report.importedProductCount + report.skippedProductCount);
    auto bounds = [](const IfcModel &source) {
      glm::vec3 minimum(std::numeric_limits<float>::max()),
          maximum(std::numeric_limits<float>::lowest());
      for (const auto &element : source.elements) {
        const auto range =
            std::ranges::find_if(source.meshRanges, [&](const auto &r) {
              return r.meshId == element.meshId;
            });
        if (range == source.meshRanges.end())
          continue;
        for (size_t i = range->firstIndex;
             i < size_t(range->firstIndex) + range->indexCount; ++i) {
          const auto world = glm::vec3(
              element.transform *
              glm::vec4(source.vertices[source.indices[i]].position, 1.0f));
          minimum = glm::min(minimum, world);
          maximum = glm::max(maximum, world);
        }
      }
      return std::array{minimum, maximum};
    };
    const auto worldBounds = bounds(model);
    auto sidecar = path;
    sidecar.replace_extension(".ifcx");
    const auto prepared = container::geometry::ifcx::LoadFromFile(sidecar);
    ASSERT_FALSE(prepared.elements.empty()) << sidecar;
    const auto preparedBounds = bounds(prepared);
    if (std::string_view(relative).starts_with("Tekla")) {
      using Triangle = std::array<glm::dvec3, 3>;
      const auto uuid = [](std::string_view guid) {
        constexpr std::string_view alphabet =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";
        std::array<uint8_t, 16> bytes{};
        for (char c : guid) {
          uint32_t carry = static_cast<uint32_t>(alphabet.find(c));
          for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
            carry += uint32_t{*it} << 6;
            *it = static_cast<uint8_t>(carry);
            carry >>= 8;
          }
        }
        constexpr std::string_view hex = "0123456789abcdef";
        std::string result;
        for (size_t i = 0; i < bytes.size(); ++i) {
          if (i == 4 || i == 6 || i == 8 || i == 10)
            result += '-';
          result += hex[bytes[i] >> 4];
          result += hex[bytes[i] & 15];
        }
        return result;
      };
      // IFCX paths use the expanded UUID of the same STEP product. Compare
      // body surfaces, excluding IFCX's separately represented axis curves.
      const auto triangles = [](const IfcModel &source, std::string_view id,
                                bool prepared) {
        std::vector<Triangle> result;
        for (const auto &element : source.elements) {
          if (element.geometryKind !=
                  container::geometry::dotbim::GeometryKind::Mesh ||
              (prepared ? element.guid.find(id) == std::string::npos
                        : element.guid != id))
            continue;
          const auto range = std::ranges::find(
              source.meshRanges, element.meshId,
              &container::geometry::dotbim::MeshRange::meshId);
          if (range == source.meshRanges.end())
            continue;
          for (size_t i = range->firstIndex;
               i < size_t(range->firstIndex) + range->indexCount; i += 3) {
            Triangle t;
            for (size_t j = 0; j < 3; ++j)
              t[j] = glm::dvec3(
                  element.transform *
                  glm::vec4(source.vertices[source.indices[i + j]].position,
                            1));
            result.push_back(t);
          }
        }
        return result;
      };
      const auto onSurface = [](const glm::dvec3 &p,
                                const std::vector<Triangle> &surface) {
        // Two independently tessellated curved surfaces have a combined
        // chord error budget of 2 mm. Planar fixture checks are tighter.
        constexpr double tolerance = .002;
        for (const auto &t : surface) {
          const auto minimum = glm::min(t[0], glm::min(t[1], t[2])),
                     maximum = glm::max(t[0], glm::max(t[1], t[2]));
          if (glm::any(glm::lessThan(p, minimum - tolerance)) ||
              glm::any(glm::greaterThan(p, maximum + tolerance)))
            continue;
          const auto a = t[1] - t[0], b = t[2] - t[0], d = p - t[0];
          const auto n = glm::cross(a, b);
          const double nn = glm::dot(n, n), aa = glm::dot(a, a),
                       ab = glm::dot(a, b), bb = glm::dot(b, b);
          const double denominator = aa * bb - ab * ab;
          if (nn > 0 && denominator > 0) {
            const double u =
                (bb * glm::dot(d, a) - ab * glm::dot(d, b)) / denominator;
            const double v =
                (aa * glm::dot(d, b) - ab * glm::dot(d, a)) / denominator;
            if (u >= 0 && v >= 0 && u + v <= 1 &&
                std::pow(glm::dot(d, n), 2) <= tolerance * tolerance * nn)
              return true;
          }
          for (size_t i = 0; i < 3; ++i) {
            const auto edge = t[(i + 1) % 3] - t[i];
            const double length2 = glm::dot(edge, edge);
            const auto closest =
                t[i] +
                edge * (length2 > 0
                            ? std::clamp(glm::dot(p - t[i], edge) / length2,
                                         0.0, 1.0)
                            : 0.0);
            if (glm::length(p - closest) <= tolerance)
              return true;
          }
        }
        return false;
      };
      std::unordered_set<std::string> preparedIds;
      for (const auto &element : prepared.elements) {
        if (element.geometryKind !=
            container::geometry::dotbim::GeometryKind::Mesh)
          continue;
        for (size_t i = 0; i + 36 <= element.guid.size(); ++i)
          if (element.guid[i + 8] == '-' && element.guid[i + 13] == '-' &&
              element.guid[i + 18] == '-' && element.guid[i + 23] == '-')
            preparedIds.insert(element.guid.substr(i, 36));
      }
      size_t comparedProducts = 0;
      for (std::string_view type :
           {"IFCWALL", "IFCCOLUMN", "IFCBEAM", "IFCSLAB", "IFCFOOTING",
            "IFCREINFORCINGBAR"}) {
        const auto element =
            std::ranges::find_if(model.elements, [&](const auto &e) {
              return e.type.starts_with(type) && e.guid.size() == 22 &&
                     preparedIds.contains(uuid(e.guid));
            });
        // The IFCX export omits some reinforcement geometry entirely. It is
        // a comparison source only for products with a body in both formats.
        if (element == model.elements.end() && type == "IFCREINFORCINGBAR") {
          std::cout
              << "IFCX has no common reinforcing-bar mesh; swept-disk "
                 "geometry is verified by independent analytic fixtures\n";
          continue;
        }
        ASSERT_NE(element, model.elements.end()) << type;
        ASSERT_EQ(element->guid.size(), 22u);
        SCOPED_TRACE(std::string(type) + " " + element->guid);
        const auto native = triangles(model, element->guid, false),
                   converted = triangles(prepared, uuid(element->guid), true);
        ASSERT_FALSE(native.empty());
        ASSERT_FALSE(converted.empty());
        const auto check = [&](const auto &from, const auto &to) {
          const size_t stride = std::max<size_t>(1, (from.size() + 63) / 64);
          for (size_t i = 0; i < from.size(); i += stride) {
            EXPECT_TRUE(
                onSurface((from[i][0] + from[i][1] + from[i][2]) / 3.0, to));
            for (const auto &point : from[i])
              EXPECT_TRUE(onSurface(point, to));
          }
        };
        check(native, converted);
        check(converted, native);
        ++comparedProducts;
      }
      EXPECT_GE(comparedProducts, 5u);
    }
    if (std::string_view(relative).starts_with("Hello Wall")) {
      using Triangle = std::array<glm::dvec3, 3>;
      const auto triangles = [](const IfcModel &source, bool prepared) {
        std::vector<Triangle> result;
        for (const auto &element : source.elements) {
          if (element.type != (prepared ? "IfcWall" : "IFCWALL"))
            continue;
          const auto range = std::ranges::find(
              source.meshRanges, element.meshId,
              &container::geometry::dotbim::MeshRange::meshId);
          if (range == source.meshRanges.end())
            continue;
          glm::dvec3 minimum(1e30), maximum(-1e30);
          std::vector<Triangle> group;
          for (size_t i = range->firstIndex;
               i < size_t(range->firstIndex) + range->indexCount; i += 3) {
            Triangle triangle;
            for (size_t j = 0; j < 3; ++j) {
              triangle[j] = glm::dvec3(
                  element.transform *
                  glm::vec4(source.vertices[source.indices[i + j]].position,
                            1));
              minimum = glm::min(minimum, triangle[j]);
              maximum = glm::max(maximum, triangle[j]);
            }
            group.push_back(triangle);
          }
          // IFCX also has auxiliary wall axes and a footprint. Select the
          // common wall body by its independently known bounds in metres.
          if (glm::length(minimum - glm::dvec3(0, 0, -.1)) < 1e-5 &&
              glm::length(maximum - glm::dvec3(10, 3, 0)) < 1e-5)
            result.insert(result.end(), group.begin(), group.end());
        }
        return result;
      };
      const auto sourceWall = triangles(model, false),
                 preparedWall = triangles(prepared, true);
      ASSERT_FALSE(sourceWall.empty());
      ASSERT_FALSE(preparedWall.empty());
      const auto onSurface = [](const glm::dvec3 &point,
                                const std::vector<Triangle> &surface) {
        for (const auto &t : surface) {
          const auto a = t[1] - t[0], b = t[2] - t[0], d = point - t[0],
                     n = glm::cross(a, b);
          const double nLength = glm::length(n);
          if (nLength == 0 || std::abs(glm::dot(d, n)) > 1e-5 * nLength)
            continue;
          const double aa = glm::dot(a, a), ab = glm::dot(a, b),
                       bb = glm::dot(b, b);
          const double denominator = aa * bb - ab * ab;
          if (denominator <= 0)
            continue;
          const double u =
              (bb * glm::dot(d, a) - ab * glm::dot(d, b)) / denominator;
          const double v =
              (aa * glm::dot(d, b) - ab * glm::dot(d, a)) / denominator;
          if (u >= -1e-5 && v >= -1e-5 && u + v <= 1 + 1e-5)
            return true;
        }
        return false;
      };
      const auto check = [&](const auto &from, const auto &to) {
        for (const auto &t : from) {
          EXPECT_TRUE(onSurface((t[0] + t[1] + t[2]) / 3.0, to));
          for (size_t i = 0; i < 3; ++i) {
            EXPECT_TRUE(onSurface(t[i], to));
            EXPECT_TRUE(onSurface((t[i] + t[(i + 1) % 3]) / 2.0, to));
          }
        }
      };
      check(sourceWall, preparedWall);
      check(preparedWall, sourceWall);
    }
    if (std::string_view(relative).starts_with("Hello Wall")) {
      const auto surfaces = [](const IfcModel &source) {
        nlohmann::json result = nlohmann::json::array();
        for (const auto &element : source.elements) {
          const auto range = std::ranges::find(
              source.meshRanges, element.meshId,
              &container::geometry::dotbim::MeshRange::meshId);
          if (range == source.meshRanges.end())
            continue;
          nlohmann::json triangles = nlohmann::json::array();
          for (size_t i = range->firstIndex;
               i < size_t(range->firstIndex) + range->indexCount; i += 3) {
            auto triangle = nlohmann::json::array();
            for (size_t j = 0; j < 3; ++j) {
              const auto p = glm::vec3(
                  element.transform *
                  glm::vec4(source.vertices[source.indices[i + j]].position,
                            1));
              triangle.push_back({p.x, p.y, p.z});
            }
            triangles.push_back(std::move(triangle));
          }
          result.push_back({{"name", element.displayName},
                            {"guid", element.guid},
                            {"type", element.type},
                            {"source", element.sourceId},
                            {"triangles", triangles}});
        }
        return result;
      };
      if (const char *output = std::getenv("CONTAINER_IFC_REVIEW_REPORT")) {
        std::ofstream stream(std::filesystem::path(output).parent_path() /
                             "hello-wall-surfaces.json");
        stream << nlohmann::json{{"ifc", surfaces(model)},
                                 {"ifcx", surfaces(prepared)}}
                      .dump();
        ASSERT_TRUE(stream.good());
      }
    }
    auto vectorJson = [](const glm::vec3 &v) {
      return nlohmann::json::array({v.x, v.y, v.z});
    };
    results.push_back({{"path", relative},
                       {"status", container::geometry::importCompletenessName(
                                      report.completeness)},
                       {"sourceProducts", report.sourceProductCount},
                       {"importedProducts", report.importedProductCount},
                       {"skippedProducts", report.skippedProductCount},
                       {"partialProducts", report.partialProductCount},
                       {"warnings", report.representationWarnings},
                       {"meshGroups", model.meshRanges.size()},
                       {"nativeCurveRanges", model.nativeCurveRanges.size()},
                       {"curveInstances", curveInstances},
                       {"boundsMin", vectorJson(worldBounds[0])},
                       {"boundsMax", vectorJson(worldBounds[1])},
                       {"ifcxBoundsMin", vectorJson(preparedBounds[0])},
                       {"ifcxBoundsMax", vectorJson(preparedBounds[1])}});
    results.back()["diagnostics"] = nlohmann::json::array();
    for (const auto &d : report.diagnostics)
      results.back()["diagnostics"].push_back({{"type", d.representationType},
                                               {"entity", d.entityId},
                                               {"product", d.productId},
                                               {"guid", d.productGuid},
                                               {"reason", d.reason}});
  }
  if (const char *output = std::getenv("CONTAINER_IFC_REVIEW_REPORT")) {
    std::ofstream stream(output);
    ASSERT_TRUE(stream.good());
    stream << results.dump(2);
    ASSERT_TRUE(stream.good());
  }
  // The report is also available in CI logs when no artifact path was supplied.
  for (auto result : results) {
    result.erase("diagnostics");
    std::cout << result.dump() << '\n';
  }
}

TEST(IfcTessellatedLoader, ParsesPlacementUnitsAndIndexedColors) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#10=IFCCARTESIANPOINT((1000.,2000.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1000.,0.,0.),(0.,1000.,0.),(0.,0.,1000.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,2,3),(1,3,4)),$);
#22=IFCCOLOURRGBLIST(((1.,0.,0.),(0.,1.,0.)));
#23=IFCINDEXEDCOLOURMAP(#21,$,#22,(1,2));
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));
#40=IFCBUILDINGELEMENTPROXY('proxy-guid',$,'Proxy',$,$,#14,#31,$,$);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc, 2.0f);

  ASSERT_EQ(model.meshRanges.size(), 2u);
  ASSERT_EQ(model.elements.size(), 2u);
  EXPECT_EQ(model.vertices.size(), 6u);
  EXPECT_EQ(model.indices.size(), 6u);

  EXPECT_TRUE(model.unitMetadata.hasSourceUnits);
  EXPECT_EQ(model.unitMetadata.sourceUnits, "millimetre");
  EXPECT_TRUE(model.unitMetadata.hasMetersPerUnit);
  EXPECT_NEAR(model.unitMetadata.metersPerUnit, 0.001f, 1.0e-9f);
  EXPECT_TRUE(model.unitMetadata.hasImportScale);
  EXPECT_NEAR(model.unitMetadata.importScale, 2.0f, 1.0e-6f);
  EXPECT_TRUE(model.unitMetadata.hasEffectiveImportScale);
  EXPECT_NEAR(model.unitMetadata.effectiveImportScale, 0.002f, 1.0e-9f);

  EXPECT_NEAR(model.elements[0].transform[0].x, 0.002f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[1].z, -0.002f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[2].y, 0.002f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].y, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].z, -4.0f, 1.0e-6f);
  EXPECT_EQ(model.elements[0].guid, "proxy-guid");
  EXPECT_EQ(model.elements[0].type, "IFCBUILDINGELEMENTPROXY");

  const bool hasRed =
      (model.elements[0].color.r > 0.9f && model.elements[0].color.g < 0.1f) ||
      (model.elements[1].color.r > 0.9f && model.elements[1].color.g < 0.1f);
  const bool hasGreen =
      (model.elements[0].color.g > 0.9f && model.elements[0].color.r < 0.1f) ||
      (model.elements[1].color.g > 0.9f && model.elements[1].color.r < 0.1f);
  EXPECT_TRUE(hasRed);
  EXPECT_TRUE(hasGreen);

  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-5f);
}

TEST(IfcTessellatedLoader, ParsesMappedItemTransformAndStyleColor) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1000.,0.,0.),(0.,1000.,0.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,2,3)),$);
#22=IFCCOLOURRGB($,0.25,0.5,0.75);
#23=IFCSURFACESTYLERENDERING(#22,0.,$,$,$,$,$,$,.NOTDEFINED.);
#24=IFCSURFACESTYLE('paint',.BOTH.,(#23));
#25=IFCSTYLEDITEM(#21,(#24),$);
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCREPRESENTATIONMAP(#13,#30);
#40=IFCDIRECTION((1.,0.,0.));
#41=IFCDIRECTION((0.,1.,0.));
#42=IFCCARTESIANPOINT((1000.,0.,0.));
#43=IFCDIRECTION((0.,0.,1.));
#44=IFCCARTESIANTRANSFORMATIONOPERATOR3D(#40,#41,#42,1.,#43);
#50=IFCMAPPEDITEM(#31,#44);
#51=IFCSHAPEREPRESENTATION($,'Body','MappedRepresentation',(#50));
#52=IFCPRODUCTDEFINITIONSHAPE($,$,(#51));
#60=IFCBUILDINGELEMENTPROXY('mapped-guid',$,'Proxy',$,$,#14,#52,$,$);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.elements[0].transform[3].x, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.r, 0.25f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.5f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.75f, 1.0e-6f);
}

TEST(IfcTessellatedLoader, PreservesProductStoreyAndMaterialMetadata) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,2,3)),$);
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));
#40=IFCBUILDINGELEMENTPROXY('proxy-guid',$,'Proxy Name',$,'Proxy Object Type',#14,#31,$,$);
#50=IFCBUILDINGSTOREY('storey-guid',$,'Level 01',$,$,$,$,$,$);
#51=IFCRELCONTAINEDINSPATIALSTRUCTURE('containment-guid',$,$,$,(#40),#50);
#60=IFCMATERIAL('Concrete',$,'Structural');
#61=IFCRELASSOCIATESMATERIAL('material-guid',$,$,$,(#40),#60);
#70=IFCPROPERTYSINGLEVALUE('Discipline',$,IFCLABEL('Architecture'),$);
#71=IFCPROPERTYSINGLEVALUE('Phase',$,IFCLABEL('New construction'),$);
#72=IFCPROPERTYSINGLEVALUE('FireRating',$,IFCLABEL('2h'),$);
#73=IFCPROPERTYSINGLEVALUE('LoadBearing',$,IFCBOOLEAN(.T.),$);
#74=IFCPROPERTYSINGLEVALUE('Status',$,IFCLABEL('Existing'),$);
#77=IFCPROPERTYSINGLEVALUE('AcousticRating',$,IFCLABEL('Rw40'),$);
#78=IFCPROPERTYENUMERATEDVALUE('Combustible',$,(IFCLABEL('No')),$);
#75=IFCPROPERTYSET('pset-guid',$,'Pset_WallCommon',$,(#70,#71,#72,#73,#74,#77,#78));
#76=IFCRELDEFINESBYPROPERTIES('props-guid',$,$,$,(#40),#75);
#80=IFCQUANTITYLENGTH('Height',$,$,3.5,$);
#81=IFCQUANTITYAREA('GrossSideArea',$,$,12.25,$);
#82=IFCELEMENTQUANTITY('quantity-guid',$,'BaseQuantities',$,$,(#80,#81));
#83=IFCRELDEFINESBYPROPERTIES('quantity-rel-guid',$,$,$,(#40),#82);
#90=IFCCLASSIFICATION($,$,$,'Uniclass2015',$,$,$);
#91=IFCCLASSIFICATIONREFERENCE($,'Ss_25_10_30','Wall classification',#90);
#92=IFCRELASSOCIATESCLASSIFICATION('class-rel-guid',$,$,$,(#40),#91);
#95=IFCSYSTEM('system-guid',$,'HVAC System',$,$);
#96=IFCRELASSIGNSTOGROUP('system-rel-guid',$,$,$,(#40),$,#95);
#97=IFCZONE('zone-guid',$,'Thermal Zone',$,$);
#98=IFCRELASSIGNSTOGROUP('zone-rel-guid',$,$,$,(#40),$,#97);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc);

  ASSERT_EQ(model.elements.size(), 1u);
  const auto &element = model.elements[0];
  EXPECT_EQ(element.guid, "proxy-guid");
  EXPECT_EQ(element.type, "IFCBUILDINGELEMENTPROXY");
  EXPECT_EQ(element.displayName, "Proxy Name");
  EXPECT_EQ(element.objectType, "Proxy Object Type");
  EXPECT_EQ(element.storeyId, "storey-guid");
  EXPECT_EQ(element.storeyName, "Level 01");
  EXPECT_EQ(element.materialName, "Concrete");
  EXPECT_EQ(element.materialCategory, "Structural");
  EXPECT_EQ(element.discipline, "Architecture");
  EXPECT_EQ(element.phase, "New construction");
  EXPECT_EQ(element.fireRating, "2h");
  EXPECT_EQ(element.loadBearing, "true");
  EXPECT_EQ(element.status, "Existing");
  EXPECT_EQ(element.sourceId, "#40");

  const auto findProperty = [&](std::string_view set, std::string_view name,
                                std::string_view category) {
    return std::ranges::find_if(element.properties, [&](const auto &property) {
      return property.set == set && property.name == name &&
             property.category == category;
    });
  };
  const auto acoustic =
      findProperty("Pset_WallCommon", "AcousticRating", "pset");
  ASSERT_NE(acoustic, element.properties.end());
  EXPECT_EQ(acoustic->value, "Rw40");
  const auto combustible =
      findProperty("Pset_WallCommon", "Combustible", "pset");
  ASSERT_NE(combustible, element.properties.end());
  EXPECT_EQ(combustible->value, "No");
  const auto height = findProperty("BaseQuantities", "Height", "quantity");
  ASSERT_NE(height, element.properties.end());
  EXPECT_EQ(height->value, "3.500000");
  const auto grossSideArea =
      findProperty("BaseQuantities", "GrossSideArea", "quantity");
  ASSERT_NE(grossSideArea, element.properties.end());
  EXPECT_EQ(grossSideArea->value, "12.250000");
  const auto classification =
      findProperty("Uniclass2015", "Wall classification", "classification");
  ASSERT_NE(classification, element.properties.end());
  EXPECT_EQ(classification->value, "Ss_25_10_30");
  const auto system = findProperty("IFCSYSTEM", "System", "reference");
  ASSERT_NE(system, element.properties.end());
  EXPECT_EQ(system->value, "HVAC System");
  const auto zone = findProperty("IFCZONE", "Zone", "reference");
  ASSERT_NE(zone, element.properties.end());
  EXPECT_EQ(zone->value, "Thermal Zone");
  const auto storeyReference = findProperty(
      "IFCRELCONTAINEDINSPATIALSTRUCTURE", "BuildingStorey", "reference");
  ASSERT_NE(storeyReference, element.properties.end());
  EXPECT_EQ(storeyReference->value, "Level 01");
}

TEST(IfcTessellatedLoader, PreservesBrowsingRelationshipAndMaterialMetadata) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,2,3)),$);
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));
#40=IFCWALL('wall-guid',$,'Wall Instance',$,'Wall Occurrence',#14,#31,$,$);
#44=IFCBEAM('beam-guid',$,'Beam Instance',$,$,#14,#31,$,$);
#70=IFCPROPERTYSINGLEVALUE('TypeMark',$,IFCLABEL('WT-01'),$);
#71=IFCPROPERTYSET('type-pset-guid',$,'Pset_TypeCommon',$,(#70));
#72=IFCWALLTYPE('wall-type-guid',$,'Wall Type A',$,$,(#71),$,$,.STANDARD.);
#73=IFCRELDEFINESBYTYPE('type-rel-guid',$,$,$,(#40),#72);
#80=IFCMATERIAL('Gypsum Board',$,'Finish');
#81=IFCMATERIAL('Concrete',$,'Core');
#82=IFCMATERIALLAYER(#80,0.012,$,'Interior Finish',$,'Finish',1);
#83=IFCMATERIALLAYER(#81,0.200,$,'Concrete Core',$,'Core',2);
#84=IFCMATERIALLAYERSET((#82,#83),'Wall Layer Set',$);
#85=IFCMATERIALLAYERSETUSAGE(#84,.AXIS2.,.POSITIVE.,0.0,$);
#86=IFCRELASSOCIATESMATERIAL('wall-material-rel-guid',$,$,$,(#72),#85);
#90=IFCMATERIAL('Steel',$,'Metal');
#91=IFCCIRCLEPROFILEDEF(.AREA.,'Round Profile',$,0.1);
#92=IFCMATERIALPROFILE('Primary Profile',$,#90,#91,$,'Structural');
#93=IFCMATERIALPROFILESET('Beam Profile Set',$,(#92),$);
#94=IFCMATERIALPROFILESETUSAGE(#93,5,2.5);
#95=IFCRELASSOCIATESMATERIAL('beam-material-rel-guid',$,$,$,(#44),#94);
#100=IFCBUILDINGELEMENTPROXY('assembly-guid',$,'Assembly',$,$,#14,#31,$,$);
#101=IFCRELAGGREGATES('aggregate-rel-guid',$,$,$,#100,(#40,#44));
#102=IFCBUILDINGELEMENTPROXY('nested-guid',$,'Nested Part',$,$,#14,#31,$,$);
#103=IFCRELNESTS('nest-rel-guid',$,$,$,#40,(#102));
#110=IFCOPENINGELEMENT('opening-guid',$,'Window Opening',$,$,#14,#31,$);
#111=IFCRELVOIDSELEMENT('void-rel-guid',$,$,$,#40,#110);
#112=IFCWINDOW('window-guid',$,'Window',$,$,#14,#31,$,$);
#113=IFCRELFILLSELEMENT('fill-rel-guid',$,$,$,#110,#112);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc);

  ASSERT_EQ(model.elements.size(), 5u);
  const auto findElement = [&](std::string_view guid) {
    return std::ranges::find_if(model.elements, [&](const auto &element) {
      return element.guid == guid;
    });
  };
  const auto hasProperty = [](const auto &element, std::string_view set,
                              std::string_view name,
                              std::string_view category,
                              std::string_view value) {
    return std::ranges::any_of(element.properties, [&](const auto &property) {
      return property.set == set && property.name == name &&
             property.category == category && property.value == value;
    });
  };

  const auto wall = findElement("wall-guid");
  ASSERT_NE(wall, model.elements.end());
  EXPECT_EQ(wall->materialName, "Gypsum Board");
  EXPECT_TRUE(hasProperty(*wall, "IFCRELDEFINESBYTYPE", "Type",
                          "relationship", "Wall Type A"));
  EXPECT_TRUE(hasProperty(*wall, "Pset_TypeCommon", "TypeMark", "pset",
                          "WT-01"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELASSOCIATESMATERIAL", "Material",
                          "relationship", "Gypsum Board"));
  EXPECT_TRUE(hasProperty(*wall, "IFCMATERIALLAYERSETUSAGE", "LayerSet",
                          "material", "Wall Layer Set"));
  EXPECT_TRUE(hasProperty(*wall, "IFCMATERIALLAYERSET", "Layer.2", "material",
                          "Concrete"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELAGGREGATES", "Parent",
                          "relationship", "Assembly"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELNESTS", "Child.1", "relationship",
                          "Nested Part"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELVOIDSELEMENT", "Opening",
                          "relationship", "Window Opening"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELFILLSELEMENT", "FilledBy",
                          "relationship", "Window"));

  const auto beam = findElement("beam-guid");
  ASSERT_NE(beam, model.elements.end());
  EXPECT_TRUE(hasProperty(*beam, "IFCMATERIALPROFILESETUSAGE", "ProfileSet",
                          "material", "Beam Profile Set"));
  EXPECT_TRUE(hasProperty(*beam, "IFCMATERIALPROFILESET",
                          "Profile.1.Material", "material", "Steel"));
  EXPECT_TRUE(hasProperty(*beam, "IFCRELAGGREGATES", "Parent",
                          "relationship", "Assembly"));

  const auto assembly = findElement("assembly-guid");
  ASSERT_NE(assembly, model.elements.end());
  EXPECT_TRUE(hasProperty(*assembly, "IFCRELAGGREGATES", "Child.1",
                          "relationship", "Wall Instance"));

  const auto nested = findElement("nested-guid");
  ASSERT_NE(nested, model.elements.end());
  EXPECT_TRUE(hasProperty(*nested, "IFCRELNESTS", "Parent", "relationship",
                          "Wall Instance"));

  const auto window = findElement("window-guid");
  ASSERT_NE(window, model.elements.end());
  EXPECT_TRUE(hasProperty(*window, "IFCRELFILLSELEMENT", "Opening",
                          "relationship", "Window Opening"));
  EXPECT_TRUE(hasProperty(*window, "IFCRELFILLSELEMENT", "VoidsElement",
                          "relationship", "Wall Instance"));
}

TEST(IfcTessellatedLoader, AppliesRectangularOpeningVoidToSweptSolidWall) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINT((0.,0.,0.));
#21=IFCCARTESIANPOINT((1000.,0.,0.));
#22=IFCCARTESIANPOINT((1000.,100.,0.));
#23=IFCCARTESIANPOINT((0.,100.,0.));
#24=IFCPOLYLINE((#20,#21,#22,#23,#20));
#25=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#24);
#26=IFCEXTRUDEDAREASOLID(#25,#13,#11,1000.);
#27=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#26));
#28=IFCPRODUCTDEFINITIONSHAPE($,$,(#27));
#29=IFCWALL('wall-guid',$,$,$,$,#14,#28,$,$);
#30=IFCCARTESIANPOINT((250.,0.,250.));
#31=IFCAXIS2PLACEMENT3D(#30,#11,#12);
#32=IFCLOCALPLACEMENT(#14,#31);
#40=IFCCARTESIANPOINT((0.,0.,0.));
#41=IFCCARTESIANPOINT((500.,0.,0.));
#42=IFCCARTESIANPOINT((500.,100.,0.));
#43=IFCCARTESIANPOINT((0.,100.,0.));
#44=IFCPOLYLINE((#40,#41,#42,#43,#40));
#45=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#44);
#46=IFCEXTRUDEDAREASOLID(#45,#13,#11,500.);
#47=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#46));
#48=IFCPRODUCTDEFINITIONSHAPE($,$,(#47));
#49=IFCOPENINGELEMENT('opening-guid',$,$,$,$,#32,#48,$);
#50=IFCRELVOIDSELEMENT('void-guid',$,$,$,#29,#49);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].type, "IFCWALL");
  EXPECT_EQ(model.elements[0].guid, "wall-guid");
  const auto wallRangeIt =
      std::ranges::find_if(model.meshRanges, [&](const auto &range) {
        return range.meshId == model.elements[0].meshId;
      });
  ASSERT_NE(wallRangeIt, model.meshRanges.end());
  EXPECT_GT(wallRangeIt->indexCount, 36u);
}

} // namespace
