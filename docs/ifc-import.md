# STEP IFC import coverage

The native implementation for [#61](https://github.com/semihguresci/VulkanSceneRenderer/issues/61)
adds conversion, solid operations and explicit import diagnostics. It is a
subset of IFC geometry, with a Manifold mesh Boolean kernel.

## Import results

`dotbim::Model::importReport` contains the STEP importer result. The BIM manager
retains it for the inspector, model-load status, logs and capture telemetry.

| Result | Meaning |
| --- | --- |
| Complete | Represented products produced drawable geometry without traversal/conversion warnings. |
| Partial | Usable geometry was retained, but some product shapes were omitted or could not be evaluated. |
| Failed | No drawable product geometry was produced. Loading fails with a diagnostic instead of opening an empty scene. |
| Unreported | Another loader did not supply this STEP coverage report; it does not imply completeness. |

Counts refer to products with a product definition shape, excluding opening
elements that are subtracted from their hosts. One product may generate several
color groups, and one representation may be instantiated by several products.
`importedProducts + skippedProducts = sourceProducts`; `partialProducts` is a
subset of imported products. Warning counts are distinct product/entity pairs,
not a count of unique source entities or missing rendered objects.

Each warning records the representation type, STEP entity ID, product ID, GUID
and reason. Failed extrusion diagnostics also identify their profile entity.
The inspector exposes all warnings in a scrolling list; screenshot/GFXReconstruct
telemetry contains aggregates to avoid copying thousands of diagnostics each
frame. The optional sample review test exports the full diagnostic inventory.

An entirely unsupported product cannot fall back to displaying its raw Boolean
operands as unplaced geometry. Existing prepared IFCX sidecar recovery remains
available after a failed STEP import, but the report explicitly identifies the
failed STEP import and the IFCX file being displayed.

Runtime failure checks verify both paths: a Boolean-only product with open, non-solid operands
exits with its shape/product IDs in the error; the same source with a prepared
sidecar displays IFCX geometry while retaining the failed STEP result. Both
checks are free of validation and synchronization errors.

## Representation coverage

| Representation | Coverage and limits |
| --- | --- |
| `IFCTRIANGULATEDFACESET` | Triangles, indexed colors and optional `PnIndex` remapping. Invalid coordinate/index rows reject the face set atomically. |
| `IFCPOLYGONALFACESET` | Planar convex/concave faces, indexed polygonal faces with voids, indexed colors, optional `PnIndex`. Authored outer-face winding is retained. |
| `IFCFACETEDBREP` | Connected closed shells with planar `IFCFACE`/polyloop bounds, inner loops, bound orientation and per-face surface colors. Closed topology and outward shell orientation are checked before buffer mutation. |
| `IFCFACETEDBREPWITHVOIDS` | Faceted outer shells with inward-facing cavity shells. Cavities must be strictly enclosed, disjoint and unnested. Repeated shells/faces, open shells and incorrect winding reject atomically. |
| `IFCADVANCEDBREP`, `IFCADVANCEDBREPWITHVOIDS` | Planar and regular curved `IFCADVANCEDFACE` geometry on planes, cylinders, spheres, tori, explicit-knot polynomial/rational spline surfaces and supported extrusion/revolution surfaces. Shared `IFCEDGECURVE`/`IFCORIENTEDEDGE` topology, face holes, periodic bands with independent seams, seam curves and the same cavity validation are retained. Spherical pole `IFCVERTEXLOOP` bounds mesh closed spheres and caps. Edge vertices trim their native 3D curves; SameSense, boundary orientation, surface membership and opposite edge reuse are checked. Other singular charts and some periodic charts remain unsupported. |
| `IFCEXTRUDEDAREASOLID` | Arbitrary supported closed 2D curves, inner void loops, rectangles, circles/hollow circles, and L/U/I profiles with signed slopes, fillet/edge radii and 2D placements. Project plane-angle units are honored; invalid tapers and overlapping fillets reject atomically. Caps and side walls face outward, including negative extrusion directions. |
| Profile curves | Closed 2D polylines/indexed arcs, conics, B-splines, trims and composites use the shared curve evaluator, with at most 4,097 sampled points. Repeated consecutive polyline points are removed. Profiles must pass planarity, closure, intersection and triangulation checks. |
| Opening relations | Analytic rectangular through-openings, with Manifold subtraction for supported closed solids including circular, rotated and overlapping cuts. Source placements/mapped transforms are applied in host coordinates. Failed cuts retain the original host with diagnostics; cut faces inherit the host material. |
| Mapped instances | Existing representation maps, local placements, uniform/non-uniform and mirrored transforms. Units, source IDs, GUIDs, semantic metadata and supported surface colors are retained. |
| Boolean/clipping results | Nested union/difference/intersection of supported closed, oriented mesh solids; plane, boxed and polygon-bounded half-space difference/intersection honors IFC agreement flags. Cycles, invalid operators, open/nonmanifold operands and unbounded unions are rejected with diagnostics. Styled results override operand colors; otherwise source face colors are retained. |
| `IfcSweptDiskSolid` | Solid/hollow disks on bounded 3D curves, including splines, closed loops and mitered C0 joins. Explicit extents retain the directrix's source parameter domain. Straight and open tangential line/arc paths retain their analytic conversion. Disconnected paths, detected tube overlaps, curvature folds, turns above 120 degrees and segments too short for their miters reject atomically. |
| `IfcSweptDiskSolidPolygonal` | Polyline or omitted-segment indexed-polycurve directrices, optional circular fillets, hollow walls and closed seams. Omitted fillets retain mitered joins. Radius/segment constraints and tangent-extent overlap are checked; adjacent fillets may meet exactly. |
| `IfcSectionedSurface` | Drawable open meshes and pcurves between placed open cross sections on a 3D directrix. Arbitrary open curves and width/slope profiles are supported. Ordered tag runs support splits, merges and multiway branches using linear station interpolation. Planar piecewise-linear sharp joins use shared half-angle miters. Guide curves, crossing/reordered tags and other sharp-join configurations remain unsupported. |
| Other parameterized profiles | Unimplemented profile families are identified through extruded-solid diagnostics. |
| Auxiliary curves | Native Vulkan line lists for the concrete IFC 4.3 curve families listed below, including curves inside `IFCGEOMETRICCURVESET`. Closed paths retain their final segment; independent paths remain separate. Mapped instances, placements, units, curve colors and product identity are retained. Unbounded curves require explicit trims or segments. |

Polygon triangulation uses Mapbox Earcut, a header-only vcpkg dependency
(`earcut-hpp`, tested with 3.2.4). It is ISC licensed; Windows packages include
`Earcut-ISC.txt`. Before triangulation, the importer checks finite coordinates,
planarity, loop intersections and hole containment. Area checks reject an
incomplete triangulation. This is not a general IFC schema or solid-manifold
validator. Loop intersection checks are quadratic in the number of face vertices.

Solid operations use Manifold 3.5.4 (Apache-2.0) and its vcpkg dependency
Clipper2 2.0.1 (Boost-1.0). Flattened mesh positions are welded exactly before
Manifold validates topology. This does not repair arbitrary self-intersecting
input solids. IFC vertices are still stored as floats; intersection faces that
collapse to zero area at that precision have no drawable surface and are removed.
Unsupported or nonfinite geometry is rejected before buffer mutation.

B-rep validation retains source face colors and authored shell winding. Shared
advanced edges use a single cached sample sequence (at most 4,097 points per
edge, sampled at 0.5 mm to reserve rounding/chart interpolation margin within the
face's 1 mm chord target). Triangulation restores omitted collinear boundary
samples to prevent cracks between adjoining faces. Faceted face bounds are
unordered; an enclosing loop is identified geometrically when no outer bound
is explicitly marked. Conversion budgets allow
8,192 boundary vertices per face, one million shared edge samples, one million
triangles and 128 cavity shells. Cavities use solid containment and boundary-gap
checks with a clearance of at least 0.1 micrometres or one ten-millionth of the
outer-shell extent. This mesh validation does not certify arbitrary geometric
self-intersections.

Curved advanced faces reuse the pcurve surface evaluators. Elementary surfaces
have analytic coordinate inverses; bounded spline/revolution charts use a
multistart Newton projection with domain clamps and residual checks. Detected
ambiguous roots and singular parameters are diagnosed. Spline `UClosed`/`VClosed`
flags do not establish a period; their domains come from the authored knots.
Face bounds unwrap angular seams before constrained UV triangulation. Collinear
boundary samples are restored, and interior edge splits update both incident
triangles. Local edge flips improve triangle quality without moving source
boundaries. Quarter/centroid and knot-line probes target a 1 mm physical chord
error; this is a sampled error target rather than a universal spline certificate.
Curved faces store normalized surface differential normals per vertex, preserving
smooth shading independently of the triangulation. Planar/faceted faces retain
their flat normals.

A closed periodic band can omit an outer bound when its two loops wind once
in opposite directions. Their seam locations need not match: a possibly slanted
cut joins existing samples, with its two sides separated by exactly one period
in surface coordinates. Matching cyclic sampling uses an adaptively sampled
interior grid; unequal sample counts use a zipper triangulation. Artificial cut
edges refine in pairs with identical float positions on both sides. Either
surface axis can carry the period, and reversed surface sense is retained.
Explicit seam bounds are also accepted; `IfcSeamCurve` requires
two distinct pcurves on the same surface and honors its master representation.
The converter retains the exact shared cap/side boundary samples. Multiple
winding bands and ambiguous periodic charts require further conversion work.
Per-chart budgets allow 131,072 vertices,
524,288 triangle slots, four million surface evaluations and eight million
boundary/quality checks. Refinement that cannot meet the chord target at renderer
float precision is rejected atomically. Surface and periodic-face semantics follow
the buildingSMART [advanced-face definition](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcAdvancedFace.htm)
and [seam-curve definition](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSeamCurve.htm).

Spherical faces also accept one `IfcVertexLoop` at either placed surface pole.
A face with only that degenerate bound covers the closed sphere; an additional
edge loop describes a pole cap and must wind once monotonically around the
selected pole. Regular stereographic charts provide smooth pole normals without
singular angular derivatives. Closed spheres use two hemispheres with an exact
shared equator. Caps join their original ring to an interior latitude using a
periodic band, so caps extending past the equator retain their source edges.
The authored pole remains a mesh vertex. Placement, degree/millimetre units,
boundary orientation and inward cavity normals are preserved. The same shell
checks and one-million-triangle B-rep budget apply. Non-pole vertex loops,
multiple vertex-loop bounds, non-spherical singular charts and edge loops
passing through a pole remain diagnosed. The source topology and angular/pole
semantics follow the buildingSMART [vertex-loop](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcVertexLoop.htm)
and [spherical-surface](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSphericalSurface.htm) definitions.

Sloped L/U/I profiles keep their bounding-box-centred coordinate systems. L-leg
and U-flange thicknesses use the profile coordinate axes as reference stations;
symmetric I-flange thickness is specified at the free edges. The two sloped
inner L faces are intersected before applying tangent fillets. Project angle
units apply to slopes; circular arc construction uses radians internally.
The dimensions and slope semantics were checked against the buildingSMART
[L](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcLShapeProfileDef.htm),
[U](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcUShapeProfileDef.htm)
and [I](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcIShapeProfileDef.htm)
definitions and the reference
[conversion formulas](https://github.com/IfcOpenShell/IfcOpenShell/tree/v0.8.0/src/ifcgeom/mapping).
No additional library was introduced for these features.

The archived 2014 buildingSMART `basin-faceted-brep.ifc` now reports its invalid
closed shell (`#851`, B-rep `#852`). An independent inspection finds 120 repeated
directed boundary edges and three unmatched edges in both the source loops and
the resulting triangles. The importer retains that source winding and rejects
the solid; its tessellated counterpart remains drawable. The optional regression
fixture uses `CONTAINER_IFC_BREP_SAMPLE_ROOT` with both basin files from
[the pinned community sample archive](https://github.com/buildingsmart-community/Community-Sample-Test-Files/tree/7ddf57a201f88a0c213d5322b02ed15e94a60a40).
This stricter check can reject malformed B-reps that previously displayed as
unvalidated surfaces, with the source product and shell identified in the report.

Circular profiles and analytic sweep paths bound the sagitta by both 1 mm in
source physical units and 0.5% of radius, with 16–4096 segments per full circle
(rounded up to a multiple of four). Swept arcs use the outer sweep extent for
their chord budget. Excessive or malformed curves are rejected. Conversion-only
operands are removed from vertex/index buffers before upload, retaining shared
meshes used by mapped products. Native curve ranges are compacted alongside
triangle ranges, preserving the line indices.

General swept disks use parallel transport frames with adaptive centerline and
tangent sampling. Their chord target is at most 1 mm and 5% of the outer radius;
centerline probes and cross-section sagitta use a quarter of that target, with
the latter also capped at 0.5% of radius. Conversion allows at most 4,096 rings,
4,096 vertices per ring and one million triangles. Closed paths distribute frame
twist over arc distance and reuse the first ring exactly at the seam, without
end caps. Hollow paths retain inward-facing inner walls and annular open caps.
[IFC C0 joins](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSweptDiskSolid.htm)
use a bisector-plane miter; neighboring miter extents must fit their segment.
Curvature checks reject local folds, and nonlocal sampled-segment distance
checks reject detected overlaps. These checks are not an exact intersection
certificate for arbitrary composed curves and do not repair invalid solids.

[Polygonal fillets](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSweptDiskSolidPolygonal.htm)
replace corner intervals with tangent circular arcs in each bend's local plane,
including 3D bends and the closed seam. Their source segment-index domain is
retained: each removed corner interval maps monotonically onto its arc.
Explicit start/end parameters trim that modified path. Fillet radius must be
at least the disk radius and fit the specified terminal/interior segment limits.
The shared sweep builder also rejects curvature folds, including a fillet equal
to the disk radius when the inner surface collapses.

[Boxed half-space](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcBoxedHalfSpace.htm)
enclosures are validated search hints and do not impose an extra clip.
[Polygon-bounded half-spaces](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcPolygonalBoundedHalfSpace.htm)
intersect a closed boundary prism with the authored plane half-space. The prism
is oriented by its own placement and sized along its axis to cover the first
operand; its artificial ends cannot affect the Boolean result. Plane normals
must not be perpendicular to that extrusion axis. Invalid/open/intersecting
boundaries and invalid placements reject atomically. These half-spaces remain
Boolean operands rather than standalone drawable solids.

Polyline geometry uses a shared builder for IFC and IFCX. Thin triangle proxies
provide bounds and picking, while display uses the existing GPU visibility and
line-list passes in deferred and forward rendering. Proxies are excluded from
meshlet estimates, surface/shadow draws and solid opening operands. Source
curve colors are unlit and respect section clipping and pass opacity; hover and
selection keep their highlight colors. IFC curve fonts and source widths are
not imported; line width follows the renderer setting and device wide-line
support. Indexed render curves are limited to 65,536 source/sample points.
Disconnected, invalid or nonfinite paths are rejected before buffer mutation.
Curve errors identify the family and source entity and reject that conversion
atomically; valid siblings can still be retained with a partial-import report.

## IFC 4.3 curve families

The native reader now has handlers for every concrete curve family in the
[IFC 4.3 curve hierarchy](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcCurve.htm).
This is curve-family coverage, subject to the domains, input budgets and
basis-surface restrictions below; it is not complete IFC geometry conformance.
The implementation adds no curve-library dependency.

| Family | Implemented behavior |
| --- | --- |
| `IfcPolyline`, `IfcIndexedPolyCurve` | 2D/3D lines and three-point circular arcs, including the collinear fallback. |
| `IfcLine`, `IfcCircle`, `IfcEllipse` | Analytic source parameterization, placed planes, angular units and conic Cartesian inversion. Standalone lines need an authored finite extent. |
| `IfcBSplineCurveWithKnots`, `IfcRationalBSplineCurveWithKnots` | Explicit clamped/unclamped knots, positive rational weights, homogeneous de Boor evaluation and exact rational derivatives. |
| `IfcTrimmedCurve` | Parameter and Cartesian trims, master representation, reverse sense and periodic seam wrapping. Ambiguous Cartesian-only locations are rejected; a consistent authored parameter disambiguates them. |
| `IfcCompositeCurve`, `IfcCompositeCurveOnSurface`, `IfcBoundaryCurve`, `IfcOuterBoundaryCurve` | Composite/reparameterized segments, accumulated domains, reverse senses and checked positional/tangent joins. Boundary-curve subclasses retain closed-loop semantics and supply UV loops to bounded surfaces. |
| `IfcOffsetCurve2D`, `IfcOffsetCurve3D`, `IfcOffsetCurveByDistances` | Analytic tangent/normal offsets and physical-distance interpolation of lateral, vertical and longitudinal offsets. Undefined normals and tangent discontinuities are rejected. |
| `IfcPcurve`, `IfcSurfaceCurve`, `IfcIntersectionCurve`, `IfcSeamCurve` | Evaluate the authored 3D curve or preferred pcurve on the basis surfaces below. Seam curves validate distinct pcurves sharing their basis surface. Unsupported bases are diagnosed. No surface-intersection solver is provided. |
| `IfcPolynomialCurve` | Finite polynomial coefficients, analytic derivatives and bounded-interval Bezier conversion. |
| `IfcClothoid`, `IfcCosineSpiral`, `IfcSineSpiral` | IFC normalized clothoid parameters and signed curvature; sine/cosine terms use their authored segment extent. |
| `IfcSecondOrderPolynomialSpiral`, `IfcThirdOrderPolynomialSpiral`, `IfcSeventhOrderPolynomialSpiral` | Signed curvature-polynomial integration and length/parameter segment selects. These complete the six IFC 4.3 spiral classes. |
| `IfcGradientCurve`, `IfcSegmentedReferenceCurve` | Horizontal station conversion, vertical profiles, shared base domains, cant elevation/frame interpolation and end placements/markers. |
| `IfcCurveSegment` | Signed length/parameter extents, insertion placements and directed tangent normalization. `IfcAxis2PlacementLinear` uses the basis curve frame and distance-expression offsets. |

Calculations use doubles in source coordinates until conversion to renderer
floats. General render curves target a 1 mm physical chord error and at most
65,536 samples. Conics use an analytic sagitta bound; splines and polynomials use
Bezier control-hull subdivision, including collinear backtracking. Other
compositions use adaptive quarter/midpoint probes; periodic surface and spiral
guards prevent high-frequency turns from aliasing into a straight segment.
There is no universal error certificate for arbitrary surface/offset compositions.
Recursion is limited to 64 levels, traversal to 4,096 curve nodes, and source
points to 65,536. Spline degrees are limited to 32 and controls to 4,096.
Inputs exceeding evaluation, integration or sample budgets are diagnosed.

Cartesian trims use a 0.01 mm position tolerance. Composite joins allow 0.1 mm
of source rounding, below the chord target; this preserves the reviewed road
alignment's authored small gaps. Vertical polynomial coefficients retain their
station-axis grade, while other vertical segment families use their specified
length and insertion tangent. Road and railway fixtures exercise the
[gradient](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcGradientCurve.htm)
and [segmented reference](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSegmentedReferenceCurve.htm)
semantics. The same evaluator supplies native lines, profile outlines and
general swept-disk directrices.

### Pcurve basis surfaces

Basis evaluation supports planes, cylinders, spheres, tori, and explicit-knot
B-spline/rational B-spline surfaces. It also supports the following derived bases:

| Surface | Parameterization and limits |
| --- | --- |
| [IfcSurfaceOfLinearExtrusion](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSurfaceOfLinearExtrusion.htm) | `u` follows the profile; `v` scales the normalized extrusion direction multiplied by `Depth`. The `v` domain is unbounded. Surface placement is applied after evaluation. |
| [IfcSurfaceOfRevolution](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSurfaceOfRevolution.htm) | `u` rotates about the local `IfcAxis1Placement` axis using project angle units; `v` follows the profile. Surface placement is applied after rotation. |
| [IfcRectangularTrimmedSurface](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcRectangularTrimmedSurface.htm) | Local parameters start at zero and map to the authored basis ranges with their senses. Cyclic seams, degree units and nested bounded-base checks are retained. |
| [IfcCurveBoundedPlane](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcCurveBoundedPlane.htm) | Evaluates within a closed outer UV loop and outside its inner loops. |
| [IfcCurveBoundedSurface](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcCurveBoundedSurface.htm) | Boundary loops use pcurves on the matching basis. An explicit/inferred outer loop or a bounded implicit outer domain is supported, with inner holes. |
| [IfcSectionedSurface](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSectionedSurface.htm) | Native convention: `u` is the directrix source parameter and `v` runs from 0 to 1 along the aligned cross section. Profile coordinates and local axis directions interpolate by physical station along smooth spans; planar polygonal spans are ruled between source sections and shared miter anchors. Profiles share their family; untagged profiles also require matching normalized break topology. |

Swept-surface profiles support bounded arbitrary open/closed 2D curves, circles,
ellipses and rectangles, including 2D placement.
[Open cross profiles](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcOpenCrossProfileDef.htm)
retain their offset point, project angle units, and horizontal or along-slope
widths. Other profile families and void/multi-component profiles are diagnosed.
Supporting a surface as a pcurve basis does not generally add standalone surface
meshing; `IfcSectionedSurface` additionally has a dedicated mesh converter.

Sectioned surfaces allow up to 128 ordered cross sections with no position
offsets. The profile X axis faces left and Y follows the placed upward axis;
authored reference directions are retained. Widths must be nonnegative;
horizontal widths cannot use vertical slopes. Zero-width segments retain their
coincident source points, including tagged/untagged branch tips, while entirely
collapsed cross sections reject. Repeated tags form contiguous ordered runs;
all sections must retain the same ordered run names. Different run multiplicities
produce split/merge tips, including multiway branches. Refinement at the union
of source occurrence fractions retains every authored profile corner. Tagged
`v` parameters rank these refined points uniformly from 0 to 1. Source
station endpoints are preserved exactly during distance conversion. The native
UV convention above is explicit because IFC 4.3 provides construction rules
without an analytic UV formula for this class; other parameter conventions are
not inferred. Missing, introduced, crossing or reordered tag runs are diagnosed.
Guide-curve transitions are not resolved.

Sharp joins use the half-angle miter prescribed by the
[sectioned-surface construction rules](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSectionedSurface.htm).
This path requires planar piecewise-linear spans, a common perpendicular upward
axis and tangent profile normals. Both incident spans use the same miter anchor,
including when branches split or merge. Miters exceeding ten times the profile
offset, detected directrix self-intersections and reversed/folded mesh triangles
reject atomically. Curved sharp joins, nonplanar sharp joins and twisted sharp-join
frames remain unsupported. Directrix intersection checks are bounded at eight
million segment pairs; miter anchors and aligned profile points each have a
4,097-point limit. The existing reader-wide point budget also applies.

Sectioned meshes seed the grid from curve samples, authored stations and profile
breaks. Adaptive quarter-point probes measure distance to the output triangles,
so a planar patch is not subdivided solely for tangential parameter warping.
Global grid refinement retains shared interior edges without T-junctions.
The physical chord target is 1 mm, with probes using a quarter of that target.
Limits are 4,097 samples per direction, 262,144 evaluations, 16 refinement rounds
and one million triangles. This is bounded adaptive sampling, not a universal
surface-intersection or chord-error certificate.

Bounded loops undergo closure, area, intersection, nesting and domain checks,
with a total boundary budget of 4,096 sampled points. Boundary points are valid;
hole interiors are excluded. UV steps are split at boundary crossings before
membership checks, preventing small holes from falling between ordinary probes.
Periodic angular guards propagate through trimmed and swept bases. Surface
recursion shares the curve traversal limits and rejects cycles.

## Verified sample results, 2026-10-05

Native Visual Studio Release, RTX 2080 SUPER, Vulkan 1.4.325, validation and
synchronization validation enabled. buildingSMART IFC5-development revision:
`1a63082ada967c683cfacee2005f8f749c8e1b79`.

| STEP sample | Represented products | Imported | Skipped | Partial among imported | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| Hello Wall | 4 | 4 | 0 | 0 | Complete: wall/window bodies, openings, indexed axis and mapped curve sets; 11 native curve instances from 6 curve geometries. |
| Tekla House | 10,042 | 10,042 | 0 | 0 | Complete: all represented products and 43 native polyline instances from 22 curve geometries; no representation warnings. |

The first native increment imported 4,069 Tekla products and skipped 5,973.
Circular profiles, swept disks, structural profiles and Boolean conversion
recover those products. Native polylines and indexed curve sets remove the
remaining auxiliary-curve warnings.
Counts include mapped instances and differ from unique source shape occurrences.

Shared cameras for the STEP/IFCX capture pairs:

| Sample | Camera position | Target |
| --- | --- | --- |
| Hello Wall | `(12, 7, 14)` | `(5, 1.5, -2)` |
| Tekla House | `(108, 62, 70)` | `(31, 20, -15)` |

Both formats use deferred rendering, 960x540, MSAA 1, TAA/bloom off, directional
intensity 2, environment intensity 1 and exposure 0.25. Outputs are local under
`out/ifc-review/polyline-captures/`, with telemetry and logs. The four captures
complete without VUID or synchronization hazards; the existing unused vertex
attribute warnings remain. Six additional curve-only captures verify source
red/green/blue colors at zero light intensity in deferred and forward rendering,
with MSAA 1/4 and TAA (MSAA 1). Forward lighting now submits native point/curve
passes and initializes depth/color for primitive-only scenes. They also complete
without VUID or synchronization hazards. Tekla STEP now recovers the complete
represented building and closely matches the IFCX image. On this machine, its capture takes
15.02 seconds versus 8.53 seconds for IFCX; these are single-run timings including
startup, not a benchmark.

Large STEP models exposed quadratic relationship-graph insertion and synthetic
property-set lookup. Indexed edge deduplication, sorted identity merging and
label indexing limited to real objects reduce Tekla graph preparation to 1.78
seconds in the earlier review (1.86 seconds with native curves). The regression
includes 20,000 repeated identities/property sets,
duplicate relationships, distinct labels/kinds and rebuilding the graph.

Hello Wall's overall bounds differ because its authored STEP space envelope is
5 m deep while the IFCX envelope is 4 m deep, and IFCX includes auxiliary geometry.
This difference does not come from misplaced wall/window bodies. Material or
auxiliary-geometry equality is not asserted for the two files.

Independent fixture checks cover concave area, hole area and empty hole interiors,
vertical face normals, a faceted cube's area/volume/outward normals, hollow
extrusion area/volume in both directions, two wall openings, rectangle placement,
mapped mirrors/units/colors/identity, malformed indices and mixed/failed imports.
Polyline fixtures additionally check closure, path separation, mapped color
inheritance, 3D circular arc sense/middle points, collinear fallback, atomic
rejection, source identity, index compaction and opening subtraction with
auxiliary curves on both hosts and openings.
Further fixtures check circular/hollow and filleted profiles, trimmed/reversed
and circular/composite swept disks, nested Boolean volumes, half-space agreement
flags, intermediate/result styles, circular/rotated/overlapping openings and
conversion failure diagnostics. Unsupported-only runtime startup fails with an
entity/product diagnostic; prepared IFCX fallback records the failed STEP status
and sidecar source. Both checks remain free of validation hazards.
IFC, IFCX, dotbim and the available USD loader cases pass. The optional sample
review test was run separately. Ten USD cases needing additional sample assets
and the manifest sample regression case requiring the absent glTF collection
were skipped; those assets were not downloaded for this change.

The curve/surface/sweep/profile/B-rep increments pass all 129 importer/core cases with the optional
buildingSMART assets present, including every road/railway gradient curve in
the reviewed samples. Tests independently check rational ellipse-profile volume,
unclamped spline basis values, spline chord error, spiral integration, signed
segment insertion, offsets, surface parameterization, repeated-location trim
ambiguity, degree units, linear placements, source styles and malformed budgets.
New checks cover surface extrusion/revolution axes and parameters, cyclic
rectangular trims, bounded holes smaller than the probe spacing, boundary-curve
subclasses, and recursive surface rejection. Independent volume and paired-edge
checks cover solid/hollow torus seams, spline sweeps with non-unit knot domains,
nonplanar closed transport, and open/closed miter bends. Further regressions
cover curvature/overlap rejection and one-sided speed integration at C0 joins.
The filleted-sweep increment adds independent filleted sweep volumes and topology,
adjacent fillet equality, sectioned surface parameter/frame and analytic area
checks, shared mesh edges, exact terminal stations, rotated/reversed polygonal
half-space boundaries, oblique plane agreement and search-box independence.
The profile/B-rep increment adds analytic volumes for signed, unit-aware tapers
and fillets; shared line/polyline/spline edge subdivisions; unordered face bounds,
face holes and styles; millimetre-scaled cavities and Boolean operands; multiple
disjoint cavities; and rejection of incorrect orientation, broken connectivity
and touching/nested/outside void shells. The archived malformed basin has an
independent source-boundary inspection and a regression diagnostic check.
The curved-face increment adds independent cylinder/slab/torus/spline volumes,
paired shell edges, spherical and bilinear surface normals, degree/millimetre
units, non-unit knot domains, face holes, extrusion/revolution faces and explicit
seam curves. Invalid winding, off-surface boundaries, ambiguous chart inverses
and exhausted float precision reject without partial buffers. A large extruded
face checks the float rounding of its cached curve samples.
Independent seam regressions cover cylinders, near-pole spherical bands with
unequal ring sampling, annular torus caps, either periodic axis and both surface
senses; every original boundary segment remains present exactly. Pole regressions
check closed-sphere and cap volumes, rotated placements, degree/millimetre units,
paired edges, smooth pole normals and inward spherical cavities. Non-pole,
duplicate and malformed vertex loops reject atomically.
Sectioned branch regressions check split/merge and multiway source corners,
zero-width branch tips and constant collapsed strips,
metre/millimetre stations, sloped crowns and sampled triangle chord error,
curved annular area, reversed/rotated miters and combined branches at miters.
Exact-position edge uses, Euler count and a single degree-two boundary loop
independently check mesh connectivity. Reordered tags, unsafe angles, folded
patches, nonplanar sharp joins and intersecting directrices reject atomically.
The full model checks still report Hello Wall 4/4 and Tekla 10,042/10,042 complete.
All ten selected importer/BIM/forward test suites pass after relinking.

Four further captures exercise independently started cylinder/sphere/torus
boundaries, a spherical pole cap and a rotated closed sphere, plus a floor.
Deferred/forward rendering with TAA off/on at 1280x720, MSAA 1 and validation
enabled reports all six products complete and no VUID/synchronization hazards.
Camera position is `(14,14,23)`, target `(0,1,0)`, directional/environment
intensities are `2`/`0.2`, and exposure is `0.3`. Images, telemetry and results
are under `out/ifc-review/periodic-poles/`. Smooth poles, the toroidal opening
and source colors remain visible without cracks at the shared chart seams.

Four sectioned-surface captures display a split crown, a merging crown, a curved
branch and a branched polygonal miter, with four pcurve overlays and a floor.
All nine products import completely in deferred/forward rendering with TAA
off/on, 1280x720, MSAA 1 and validation enabled, without VUID/synchronization
hazards. Camera position is `(15,19,25)`, target `(0,1,0)`, directional/environment
intensities are `2`/`0.2`, and exposure is `0.3`. Shared branch tips and miter
corners remain continuous in the inspected images. Images, exported fixtures,
telemetry and results are local under `out/ifc-review/sectioned-gaps/`.

Four additional runtime captures display an eight-panel curve gallery at
1280x720 in deferred/forward rendering: MSAA 1 in both, deferred MSAA 4, and
forward TAA with MSAA 1. Camera position is `(0,0,15)`, target `(0,0,0)`, lighting
intensities are zero and exposure is 1. All eight products import completely,
all panels remain visible with source colors, and no VUID/synchronization
hazards appear. Local images, telemetry, logs and results are under
`out/ifc-review/curve-families/`; these generated review artifacts are not packaged.

Four more captures exercise a six-product sweep gallery in deferred and forward
rendering, each with TAA off and on, at 1280x720 and MSAA 1. The scene contains
a hollow torus, a closed rational spline tube, an open 3D hollow
spline, a hollow right-angle miter, and a surface-extrusion pcurve helix. Camera
position is `(0,9,13)`, target `(0,0,2)`, directional/environment intensities are
2/1 and exposure is 0.5. All six products import completely, the sweeps remain
visible without seam caps, and no VUID/synchronization hazards appear. Local
images, telemetry, logs and results are under `out/ifc-review/remaining-curves/`.

The next-feature gallery adds closed and 3D hollow polygonal fillets, varying
and curved sectioned surface meshes, and an oblique polygon-bounded Boolean cut.
Four captures use deferred/forward rendering with TAA off/on, 1280x720, MSAA 1,
directional/environment intensities 2/1 and exposure 0.5. Camera position is
`(0,10,16)`, target `(0,0,-1)`. All six products import completely without
VUID/synchronization hazards. Evidence is local under `out/ifc-review/next-features/`.

The profile/B-rep gallery contains filleted sloped L/U/I sections, a cut-open
faceted cavity and a planar advanced B-rep with a through-hole. Four captures use
the same render settings as above, with camera `(0,11,17)` and target `(0,1,0)`.
All six products import completely in both techniques, with and without TAA,
and no VUID/synchronization hazards appear. Images, telemetry, logs and results
are local under `out/ifc-review/brep-profiles/`. The Visual Studio Release build
and ten selected suites pass; ten optional USD cases lack their sample assets.

The curved-face gallery displays cylindrical and spherical bands, an annular
toroidal band, a rational spline sector and a bilinear spline box. Four captures
use deferred/forward rendering with TAA off/on, 1280x720, MSAA 1, directional
intensity 2, environment intensity 0.2 and exposure 0.3. Camera position is
`(12,12,21)`, target `(0,1,0)`. All six products, including the floor, import
completely with no VUID/synchronization hazards. The surfaces shade smoothly,
the toroidal opening remains clear and the spline faces retain their curvature.
Images, telemetry, logs, exported fixtures and results are local under
`out/ifc-review/curved-brep/`.

## Reproduce

From a Visual Studio Developer Console, with manifest dependencies installed:

```powershell
cmake --build out/build/visual-studio --config Release --target VulkanSceneRenderer ifc_tessellated_loader_tests dotbim_loader_tests ifcx_loader_tests usd_loader_tests sample_model_regression_tests
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^(ifc_tessellated_loader_tests|dotbim_loader_tests|ifcx_loader_tests|usd_loader_tests|sample_model_regression_tests)$'
$env:CONTAINER_IFC_SAMPLE_ROOT = "$PWD/out/build/visual-studio/models/buildingSMART-IFC5-development/examples"
$env:CONTAINER_IFC_REVIEW_REPORT = "$PWD/out/ifc-review/import-review.json"
New-Item -ItemType Directory -Force out/ifc-review | Out-Null
& out/build/visual-studio/tests/ifc_tessellated_loader_tests.exe

# Optional: export the validated curved B-rep fixtures for interactive review.
$env:CONTAINER_IFC_CURVED_FIXTURE_ROOT = "$PWD/out/ifc-review/curved-brep/fixtures"
& out/build/visual-studio/tests/ifc_tessellated_loader_tests.exe --gtest_filter=IfcCurvedBrep.*

$exe = "$PWD/out/build/visual-studio/Release/VulkanSceneRenderer.exe"
$sample = "$env:CONTAINER_IFC_SAMPLE_ROOT/Hello Wall/hello-wall.ifc"
& $exe --model $sample --hidden --no-ui --no-taa --validation --msaa 1 --width 960 --height 540 --display-mode lit --no-bloom --directional-intensity 2 --environment-intensity 1 --exposure 0.25 --camera-position 12 7 14 --camera-target 5 1.5 -2 --screenshot "$PWD/out/ifc-review/hello-wall.png"
```

## Library choices

The native importer uses permissively licensed components. These are distinct
from a complete IFC schema and geometry conversion backend:

| Library | License | Role and tradeoff |
| --- | --- | --- |
| [Manifold](https://github.com/elalish/manifold/blob/v3.5.4/LICENSE) | Apache-2.0 | Integrated solid Boolean kernel. It requires closed, consistently oriented input meshes and does not parse IFC. |
| [Clipper2](https://github.com/AngusJohnson/Clipper2/blob/main/LICENSE) | Boost-1.0 | Integrated through Manifold for planar geometry operations. It is not an IFC reader. |
| [Mapbox Earcut](https://github.com/mapbox/earcut.hpp/blob/master/LICENSE) | ISC | Integrated planar polygon triangulation. |
| [Assimp](https://github.com/assimp/assimp/blob/master/LICENSE) | BSD-3-Clause | Has an [IFC importer](https://github.com/assimp/assimp/blob/master/code/AssetLib/IFC/IFCLoader.cpp). A possible fallback to evaluate against actual assets; its existence does not establish full modern IFC coverage or preservation of all BIM metadata. |
| [IFC++ / IfcPlusPlus](https://github.com/ifcquery/ifcplusplus/blob/master/LICENSE.txt) | MIT | An IFC reader and geometry stack. Its [maintainer describes it as largely archived](https://github.com/ifcquery/ifcplusplus), so adopting it would add maintenance work. Viewer dependencies and bundled components need review for the selected build configuration. |

[web-ifc](https://github.com/ThatOpen/engine_web-ifc/blob/main/LICENSE.md)
uses MPL-2.0 and [IfcOpenShell](https://github.com/IfcOpenShell/IfcOpenShell)
uses LGPL-3.0-or-later; these are copyleft rather than strictly permissive
licenses. The current choice keeps the renderer's native metadata/placement
handling and adds Manifold for the reviewed missing solid operations. It does
not claim complete IFC conformance.

## Remaining coverage and review

Guide-curve transitions, missing/reordered tag runs, curved or nonplanar sharp joins,
unsupported swept-surface profile families, non-spherical singular charts and
edge loops through spherical poles remain outside native coverage.
Swept-surface profile gaps include center-line, derived/mirrored, hollow and
composite definitions; swept-area solid profile support is listed separately
above.
Multiple-winding and otherwise ambiguous periodic spline charts also require
further work. Each omitted
representation is reported. A full IFC backend remains an option if future
assets need these capabilities.

The Hello Wall body surfaces agree across STEP/IFCX at sampled triangle corners,
edge midpoints and centroids in both directions within 0.01 mm, allowing
different triangulation. Tekla overall bounds agree within 0.01 mm. Representative
shared wall, column, beam, slab and footing bodies pass bidirectional corner and
centroid surface checks with a 2 mm combined chord-error budget. IFCX has no
reinforcing-bar meshes in this export; swept disks instead have independent
analytic volume and normal tests. These checks do not establish equivalence for
every product or material. #61 remains open pending review of the
implementation.
