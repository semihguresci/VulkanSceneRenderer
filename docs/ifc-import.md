# STEP IFC import coverage

The native implementation for [#61](https://github.com/semihguresci/VulkanSceneRenderer/issues/61)
adds conversion, solid operations and explicit import diagnostics. It is a
subset of IFC geometry, with a Manifold mesh Boolean kernel.

## Export a semantic scene for robotics

`BimRoboticsScene` reads native IFC STEP or IFCX on the CPU and writes a
versioned JSON manifest without opening a window. Build the tool and choose a
source namespace that stays stable for the dataset.

```powershell
cmake --build out/build/windows-release --config Release --target BimRoboticsScene
.\out\build\windows-release\BimRoboticsScene.exe "TeklaHouse.ifc" "tekla-robotics.json" "tekla-house"
```

The manifest includes authored GUIDs and source occurrence IDs, IFC classes,
product world placements, separate drawable representation placements, and
spatial relations. Project, site, building, storey, space, wall, slab, door,
and opening records can survive without drawable geometry. Native STEP
relations retain containment, aggregation, nesting, opening and filling
roles. IFCX preserves explicit composition links and any authored GUIDs. Its
stable IDs include the composition path, so repeated GUID instances stay
distinct as instances are added or removed. When a GUID is unavailable, the
ID uses the source occurrence.

Native STEP reads the project's supported length unit and composes nested
local placements. IFCX files such as Tekla House may contain neither authored
GUIDs nor length units. The manifest preserves that evidence and leaves
metric matrices `null` when the metre scale is unknown. For a source whose
scale you have verified, pass `--metres-per-renderer-unit N`. Missing or
unsupported product placements are also `null`. The loader reports unresolved
relations and ambiguous storeys under `diagnostics`. [Coordinate contract](coordinate-conventions.md#semantic-bim-export-for-robotics)

The manifest records semantic classes and placements. It carries no inferred
room boundaries, door passability, collision geometry, navigability or global
map projection. Source relations and authoring gaps remain explicit for
downstream robotics applications.

Set `CONTAINER_IFC_SAMPLE_ROOT` to the buildingSMART examples directory to run
the optional Tekla checks within `bim_robotics_scene_tests`.

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
| `IFCADVANCEDBREP`, `IFCADVANCEDBREPWITHVOIDS` | Planar and curved `IFCADVANCEDFACE` geometry on planes, cylinders, spheres, tori, explicit-knot polynomial/rational spline surfaces and supported extrusion/revolution surfaces. Shared `IFCEDGECURVE`/`IFCORIENTEDEDGE` topology, face holes, periodic bands with independent seams, seam curves and cavity validation are retained. Authored spline trims can use either periodic axis and primitive doubly periodic bands. Spherical pole vertex loops, pole-crossing edge loops and closed spherical faces with a single ordinary edge bound mesh through regular local charts. Edge vertices trim their native 3D curves. The importer checks SameSense, boundary orientation, surface membership and opposite edge reuse. |
| `IFCEXTRUDEDAREASOLID` | Arbitrary supported closed 2D curves with voids, sharp/rounded/hollow rectangles, circles/hollow circles, ellipses, trapezia, capped straight and circular-arc centerlines, disjoint area composites and L/U/I/asymmetric I/C/T/Z sections. Caps and side walls face outward, including negative extrusion directions. |
| Profile curves | Closed 2D polylines/indexed arcs, conics, B-splines, trims and composites use the shared curve evaluator, with at most 4,097 sampled points. Repeated consecutive polyline points are removed. Profiles must pass planarity, closure, intersection and triangulation checks. |
| `IfcDerivedProfileDef`, `IfcMirroredProfileDef` | Transform already-supported parent profiles after their 2D placement. Translation, rotation, uniform/non-uniform positive scales and axis mirroring are supported, including nested chains. Extruded profiles retain inner loops and outward winding; swept-surface curves retain source domains, periods and section tags. Parent/profile types must agree. |
| Opening relations | Analytic rectangular through-openings, with Manifold subtraction for supported closed solids including circular, rotated and overlapping cuts. Source placements/mapped transforms are applied in host coordinates. Failed cuts retain the original host with diagnostics; cut faces inherit the host material. |
| Mapped instances | Existing representation maps, local placements, uniform/non-uniform and mirrored transforms. Units, source IDs, GUIDs, semantic metadata and supported surface colors are retained. |
| Boolean/clipping results | Nested union/difference/intersection of supported closed, oriented mesh solids; plane, boxed and polygon-bounded half-space difference/intersection honors IFC agreement flags. Cycles, invalid operators, open/nonmanifold operands and unbounded unions are rejected with diagnostics. Styled results override operand colors; otherwise source face colors are retained. |
| `IfcSweptDiskSolid` | Solid/hollow disks on bounded 3D curves, including splines, closed loops and mitered C0 joins. Explicit extents retain the directrix's source parameter domain. Straight and open tangential line/arc paths retain their analytic conversion. Disconnected paths, detected tube overlaps, curvature folds, turns above 120 degrees and segments too short for their miters reject atomically. |
| `IfcSweptDiskSolidPolygonal` | Polyline or omitted-segment indexed-polycurve directrices, optional circular fillets, hollow walls and closed seams. Omitted fillets retain mitered joins. Radius/segment constraints and tangent-extent overlap are checked; adjacent fillets may meet exactly. |
| `IfcSectionedSurface` | Drawable open meshes and pcurves between placed open cross sections on a 3D directrix. Tagged breaklines can appear and disappear at authored zero-width tips. Guides can bind to open cross profiles, including a contiguous temporary tag run, or to indexed profile point tags. Compatible planar, nonplanar and curved sharp joins use shared half-angle miters. |
| `IfcSurfaceOfLinearExtrusion`, `IfcSurfaceOfRevolution` | Drawable open surface charts and pcurve/advanced-face bases. Centerlines, rounded rectangles, hollow circles and rectangles, and disjoint composite `CURVE` members retain their own domains and placements. A standalone revolution can include one certified smooth endpoint on the axis. |
| Auxiliary curves | Native lines and authored metric strokes for the concrete IFC 4.3 curve families listed below, including curves inside `IFCGEOMETRICCURVESET`. Closed paths retain their final segment; independent paths remain separate. Mapped instances, placements, units, source fonts, numeric model-space widths, colors and product identity are retained. Unbounded curves require explicit trims or segments. |

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
ambiguous roots and singular parameters are diagnosed. Clamped and unclamped
polynomial or rational splines can wrap on either axis when its evaluated
boundary control traces agree. The native knot span defines the period.
`UClosed` and `VClosed` are informational. Rational trace weights may differ by
a common scale. Interior knots and periodic joins use one-sided normals where
the surface tangent traces differ.
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
Explicit seam bounds require two distinct pcurves on the same surface and use
the curve's master representation. An authored spline chart checks bounded
pcurves against its exact shared edge samples. Trim crossings at sharp spline
knots add samples on the source edge before either incident face is meshed.
The importer locates these crossings with the authored pcurve or the lifted
surface inverse and gives all uses of the edge the same rounded 3D positions.
Both uses of a seam must follow
the same edge and differ by a certified period. An edge carried only by a 3D
curve can join authored pcurves when its cached samples have a consistent
surface inverse and the authored endpoints anchor its lift. The importer
checks intervening chord error and rejects half-period ties or inconsistent
anchors. A shared 3D seam must have opposite uses whose inferred lifts differ
by one certified period. An explicit face can also place an entirely inferred
outer bound or hole against its authored loops. The importer translates only
inferred loops by whole periods and requires a unique placement with all holes
strictly inside the outer bound and disjoint from one another. Doubly periodic
spline bands can follow a primitive winding on both axes, such as two U turns
and one V turn. The opposite trims can double back along their winding. Spline
bands on either single periodic axis and on both axes use a cut between existing
samples that stays between the ordered trim rails and avoids their periodic
copies. The importer checks for a single cover before refining the surface.
If a monotone band cannot cut from its first rail sample, a bounded search
tries other pairs of original rail samples. The companion of an authored
winding loop can use inferred coordinates throughout. Such a cut preserves the
original edge positions. Both copies of the artificial cut receive matching samples at
sharp knot crossings. The importer also rejects overlapping translated interiors in
explicit trims. Inconsistent lifts, intersecting periodic images and ambiguous
endpoints produce face and edge diagnostics.
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
checks and one-million-triangle B-rep budget apply. Non-pole vertex loops and
multiple vertex-loop bounds remain diagnosed.
The source topology and angular/pole
semantics follow the buildingSMART [vertex-loop](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcVertexLoop.htm)
and [spherical-surface](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSphericalSurface.htm) definitions.

An ordinary spherical edge loop that crosses a placed pole uses an oriented
stereographic chart with its excluded point outside the trimmed region.
The B-rep edge cache inserts a pole inside a native trim when it can verify
the parameter with an analytic inverse or a bounded search of nearby native
intervals. Every incident face reads the same samples. The face chart preserves
source boundary positions and native angular pcurves. The importer tests up to
six candidate charts and validates loop orientation, hole containment, chord
error and closed-shell edge reuse before publishing geometry.
The same oriented chart handles a single ordinary bound on a closed sphere
without an explicit outer bound, even when the rim misses both poles. Face
orientation selects the retained side of the rim.

A full sphere can traverse a pole-to-pole meridian in both directions.
A trimmed cap can use a pole-to-rim meridian with a single monotone rim. In
both cases, the meridian may be split across several `IfcSeamCurve` edges.
Each edge must appear twice in opposite directions within the same face bound,
and the return path must use those edges in reverse order. The importer
verifies both native pcurves on every segment and preserves the source join
vertices. The mesher partitions the face using the source seam and a second
shared meridian. For a trimmed cap the
second cut ends at an existing rim vertex. The importer changes the generated
meridian when a hole obstructs the opposite cut. It checks projected hole
edges with their chord tolerance and keeps each hole within one chart. Holes
that meet the authored seam and faces with separate meridians remain diagnosed.

Run `ifc_spherical_edge_loops_gpu_regression` with
`CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` to check shared-edge hemispheres, a
tilted cap, a sphere with a split seam and a disk filling its hole, and a
spherical face sharing its sole rim with a planar disk. The captures exercise
Forward and Deferred raster with TAA off and on.

Revolving an exact half-ellipse about a principal axis through its center gives
an analytic ellipsoid. Advanced faces on this surface use a scaled polar chart
for ordinary edge loops through either or both poles. The edge cache inserts
verified pole samples in the native trim and reuses them on both incident
faces. Analytic scaled normals preserve face sense and cavity winding. The
ellipsoid route supports ordinary edge bounds. Spherical vertex-loop and
authored meridian seam handling are described above. Run
`ifc_revolved_ellipsoid_gpu_regression` with
`CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` for analytic depth and shared-pole checks
in forward and deferred raster with TAA off and on.
[IFC surface of revolution](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSurfaceOfRevolution.htm)

An advanced face on a surface of revolution can use a Cartesian chart at a
smooth axis endpoint. Clamped polynomial and positive-weight rational splines
qualify when their projected control points prove strictly monotone radius,
their interior knots retain a continuous tangent, and the axis endpoint has
a radial tangent. The chart uses
analytic derivatives at the apex, honors the native surface sense and keeps
the original angular and profile parameters for pcurves. The edge cache
retains an apex inside a shared trim when it can verify its native parameter.
Sector faces and rims preserve their source samples. An optional apex vertex
loop becomes an exact interior vertex, including on faces with holes.

A polar seam can join the apex to a monotone rim, including a chain of
segments with opposite uses and two verified native pcurves per segment.
The same cut construction as for spherical caps selects an original rim
vertex clear of the holes. Cusps, axis crossings,
nonmonotone meridians, unsupported spline creases and composite axis-touching
profiles remain outside this coverage.

A standalone revolution or its rectangular trim uses the same endpoint
certification. When the trim includes the endpoint, the mesh shares one apex
vertex with the analytic limiting normal. The angular seam uses identical
positions at both ends of a full revolution. Standalone surfaces stay open at
the rim and at both sides of a partial angular trim. Run
`ifc_standalone_revolution_axis_gpu_regression` with
`CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` for analytic depth and coverage
comparisons in Forward and Deferred raster with TAA off and on.
Run `ifc_revolution_axis_gpu_regression` with
`CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` for the multi-span rational paraboloid in
Forward and Deferred raster with TAA off and on.

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

### Extended area profiles

The C and Z sections retain lip or flange handedness and the authored
2D placement. The Z flange width includes the web thickness. An authored C
internal radius produces concentric outer bends and square free lip ends.
With an omitted radius, IFC4 uses sharp corners and IFC2X3 uses its zero
inside-radius default. A C section must have enough girth for both lips.
[C profile](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcCShapeProfileDef.htm)
· [Z profile](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcZShapeProfileDef.htm)

The T section intersects the sloped web and flange before applying the root,
flange-tip and web-toe radii. The web thickness is measured on the horizontal
centreline and the flange thickness one quarter of the full flange width in
from each tip. The asymmetric I section retains independent top and bottom
widths, thicknesses, and IFC4 radii and slopes. IFC2X3 uses the bottom flange
values for omitted top thickness and fillet radius. Declare `FILE_SCHEMA`
for the twelve-field form when its final field is present so that the importer
can distinguish IFC2X3 centroid metadata from an IFC4 edge radius.
Signed slopes honor project angle units. When an I-section root fillet reaches
the web or flange limit, the coincident tangent stations use one vertex.
Folded sections and overlapping fillets produce profile diagnostics.
[T profile](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcTShapeProfileDef.htm)
· [I-section fillet limits](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcIShapeProfileDef.htm)
· [Asymmetric I profile](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcAsymmetricIShapeProfileDef.htm)

IFC2X3 A- and F-shaped crane rails use the authored head, web and foot stations
with straight segments between stations and the specified radius at the two crown
corners. Type F uses the common head and foot width shown in the IFC drawing.
The nominal profile is centered on the bounding box before the authored 2D
placement. Its centroid offset is metadata. Exact rolled geometry with additional
rounds or camber requires an explicit outline. Run
`ifc_legacy_crane_rails_gpu_regression` with `CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1`
to check both forms in forward and deferred raster with TAA off and on.
[IFC2X3 A rail](https://standards.buildingsmart.org/IFC/RELEASE/IFC2x3/TC1/HTML/ifcprofileresource/lexical/ifccranerailashapeprofiledef.htm)
· [IFC2X3 F rail](https://standards.buildingsmart.org/IFC/RELEASE/IFC2x3/TC1/HTML/ifcprofileresource/lexical/ifccranerailfshapeprofiledef.htm)

Hollow rectangles keep the inner and outer corner radii and the authored
wall thickness. Rounded rectangles allow radii up to the smaller half-extent.
Ellipse sampling uses the larger semiaxis for its chord bound. Trapezia keep
the signed offset of the top edge from the bottom edge's left end.
The 2D placement applies before any derived or mirrored transforms. The
curved-edge budget includes the IFC length unit and accumulated profile scales.
A later `IfcMappedItem` or user import scale scales the tessellated mesh.
[Hollow rectangle](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcRectangleHollowProfileDef.htm)
· [Trapezium](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcTrapeziumProfileDef.htm)

An AREA centerline uses a connected, open 2D curve made of straight segments
and circular arcs. Supported definitions include polylines, indexed
polylines with line and arc indices, trimmed lines and circles, and
composites of these curves. The importer forms a constant-width strip with
perpendicular ends. Straight joins can miter through a direction change of up
to 120°. Joins involving an arc must be tangent. Circular offset edges are
sampled to a 1 mm chord-error budget that includes the IFC length unit and
accumulated profile scales. The limits are 1,024 centerline points and 4,096
outline vertices. Closed or self-intersecting curves, collapsed or overlapping
offsets, and unsupported curve types produce a profile diagnostic.
[Centerline profile](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcCenterLineProfileDef.htm)

An AREA composite may contain 2–64 distinct nonrecursive region definitions
and up to 16,384 boundary vertices. Each member retains its placement and
voids. An island can sit inside another member's void. Touching, crossing or
overlapping filled areas reject the complete extrusion before it publishes
triangles. The importer keeps a valid sibling representation on the same
product and reports the failed sweep and profile IDs.
[Composite profile](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcCompositeProfileDef.htm)

Run the structural profile fixtures with
`ifc_tessellated_loader_tests --gtest_filter=IfcStructuralProfiles.*`.
The native fixture checks six rendered profiles and 17 depth samples in
Forward and Deferred raster, with TAA off and on. Enable it with
`CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` and run
`ctest -R '^ifc_area_profiles_gpu_regression$' --output-on-failure`
from the configured build directory.
Run `ifc_structural_area_profiles_gpu_regression` with the same switch for
the C, Z, T and asymmetric I profiles and a 270° centerline extrusion. The
curved profile checks cap area, rim depth, the clear bore and the opening
between the ends.

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

Polyline geometry uses a shared builder for IFC and IFCX. Default curves use
triangle pick proxies and native line lists. Default display width follows the
renderer setting, with a one-pixel fallback when the device lacks wide-line
support. Curve geometry stays outside surface and shadow passes, meshlet
estimates and solid opening operands.

[IFC model-space curve styles](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcCurveStyle.htm)
resolve explicit font patterns, predefined fonts and scale factors from styled
items and presentation style assignments. Dashed, chain, chain double dash and
dotted use the predefined millimetre segment and gap lengths, converted to the
source units before placement and mapping. [IFC predefined curve fonts](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcDraughtingPreDefinedCurveFont.htm)
· [ISO segment and gap lengths](https://steptools.com/stds/smrl/data/resource_docs/visual_presentation/sys/6_schema.htm)

Dash phase follows sampled arc length across
curve vertices and restarts at each independent path. Source units and the
mapping transform apply to both dash length and stroke width. A shared source
can be instanced with different styles. A positive numeric width uses the same
triangle stroke for display, picking, hover and selection. This supports
width-defined dots, butt-ended dashes and rounded joins in forward and deferred
rendering on devices with or without Vulkan wide lines. Source colors remain
unlit and respect curve visibility, depth and section clipping.

An IFC presentation layer can provide the model-space stroke and color of a
curve, geometric set or mapped item. Each omitted or explicit `by layer` field
inherits its layer default. An item's own width, pattern or color overrides the
corresponding default. A direct `continuous` font clears a layer's dash pattern.
Each mapped occurrence uses its own layer and scale. Conflicting layer styles
for the same item are ignored. Layer visibility and editing flags are outside
this coverage. [IFC presentation layer](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcPresentationLayerWithStyle.htm)

Paper-space dimensions require a plot scale and use the default model stroke.
External font definitions use a solid pattern. Explicit zero-length dots require
an authored or inherited numeric width.
Invalid or over-budget optional style fields fall back independently where
possible. Indexed render curves are limited to 65,536 source/sample points.
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

Swept-surface profiles support bounded arbitrary open and closed 2D curves,
circles, ellipses, sharp and rounded rectangles, hollow circles and rectangles,
and disjoint `CURVE` composites. A `CURVE` centerline uses its authored curve
and knot domain. Thickness applies to the separate swept-area solid path.
[Open cross profiles](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcOpenCrossProfileDef.htm)
retain their offset point, project angle units, and horizontal or along-slope
widths. Derived and mirrored wrappers preserve per-chart placement and domain.
Generated rounded-rectangle parameters count consecutive line and arc pieces.
Circle parameters use project angle units. Hollow boundaries and composite
members are separate charts and touching or crossing composite boundaries
produce source diagnostics. A composite accepts up to 64 members with 4,097
boundary samples per chart and 32,768 across the profile.

A multi-chart pcurve uses bounded UV geometry and the associated 3D curve or
3D topological vertices to establish a unique chart. Curve agreement uses
bidirectional chord-bounded samples and a capped search. Advanced faces use
their shared 3D edge samples to identify the chart, then honor `SameSense`.
Pcurves record sampled crossings of sharp profile joins. The face triangulation
constrains these joins and uses a normal from each side. An edge crossing a
join needs a sample at the join so adjoining faces can share its original
boundary exactly. Ambiguous or conflicting bounds identify the surface and
profile IDs in the diagnostic.

### Standalone swept surface meshes

Standalone extrusion displays one authored `Depth` along `v ∈ [0,1]`. An
explicit rectangular trim supplies its own interval. Regular revolutions
display one turn when untrimmed and use the project angle units. For standalone
display the revolution axis lies in the profile plane. The meridian stays clear
of the axis, and multi-component meridians remain on the same side. Older
single-chart pcurves can still evaluate other axis orientations. Standalone
linear sweeps require an extrusion direction out of the profile plane.
Open sheets have no end caps. Closed generating loops preserve exterior or
cavity winding after placement and reflection. Rectangular trim senses control
the parameter orientation of the resulting sheet.

The grid includes profile knots and full-period seams. It samples to a 0.25 mm
chord target and enforces evaluation, axis and triangle budgets. Unsupported or
collapsed charts reject the representation before mesh buffers change. A
standalone swept item may also appear inside an `IfcGeometricSet`.

Enable `CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` and run
`ctest -R '^ifc_swept_surface_gpu_regression$' --output-on-failure` in the build
directory. The fixture checks centerline creases, separated composite sheets,
a trimmed circular sheet and the inner and outer walls of a trimmed hollow
sweep in Forward and Deferred raster with TAA off and on.

### Derived and mirrored profiles

[Derived profiles](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcDerivedProfileDef.htm)
apply their operator after the parent's position and rotation, including parent
translations. The evaluator follows the formal
[2D base-axis rules](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcBaseAxis.htm):
Axis1 sets X and Axis2 sets the sense of its perpendicular Y; when only Axis2
is supplied, X is derived from it. Non-orthogonal authored directions supply
orientation and sense, rather than shear. Scale defaults to one; omitted Scale2
defaults to Scale. Both scales must be positive.
[Mirrored profiles](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcMirroredProfileDef.htm)
reflect X about the profile coordinate system's Y axis after the parent placement.
The Operator slot must be derived (`*` in STEP); the native parser treats omitted
and derived slots alike.

The same transform evaluator is used for swept-surface curves and extruded
profile loops. Nested transforms preserve source parameters and break points.
Cached speed, curvature and frame metadata are discarded when applying affine
transforms; derivatives transform with the scaled axes, and analytic inverses
apply their inverse. Parent sampling tolerance is divided by the largest axis
scale at each level, including circles,
structural fillets, splines and indexed arcs in area profiles. This retains the
physical chord target when profiles are enlarged. The existing reference-depth
and sampling budgets apply; invalid references/types, nonpositive scales and
nonfinite output positions reject before geometry buffers are changed.

### Sectioned profile interpolation and meshes

Sectioned surfaces allow up to 128 ordered cross sections with no position
offsets. The profile X axis faces left and Y follows the placed upward axis;
authored reference directions are retained. Widths must be nonnegative;
horizontal widths follow the direction of the slope angle and cannot use
vertical slopes. A zero-width segment retains its coincident source points.
Repeated tags form contiguous runs, and the union of their occurrence
fractions retains every authored corner during splits and merges.

An interior breakline can be absent where its first or last authored point
coincides with a unique neighboring retained occurrence. The corresponding
slot collapses onto that occurrence throughout the absence. Both tips of a
bounded absence must agree on the anchor. Reversing an entire authored row is
supported when its boundary tags and geometry establish the same orientation.
Labels may swap at a coincident tip when separated rows determine their order.
Ambiguous anchors, positive-width introductions, and crossings between
separated runs produce source profile diagnostics. [buildingSMART branching discussion](https://github.com/buildingSMART/IFC4.x-IF/issues/175)
· [Published branching pavement](https://github.com/RickBrice/IFC-Alignment-Geometry-Implementation-Guide/blob/bb522007f2adcb1e1f47a4b7934b8883c9df6036/examples/IfcSectionedSurface_with_branching.ifc)

Tagged `v` parameters rank the aligned columns uniformly from 0 to 1.
Pcurves retain birth and death stations as sampling breaks. The mesh omits
triangles collapsed across an authored section while sharing the surviving
edges with adjacent strips. The native UV convention supplies analytic
parameters for the IFC construction rules.

Tagged `IfcOffsetCurveByDistances` items bind by exact tag inside the same
product definition shape and representation frame, including its sibling
Axis or GeometricCurveSet representation. Mapped copies of an assembly use
the guides in their own matching frame. Unguided tags continue to interpolate
along the directrix. A guide on the directrix evaluates at its native station
parameter. An independent guide intersects the placed cross-section plane at
each station, with a unique transverse intersection and a consistent order
along the guide. Its authored breakpoints map back into the directrix domain
for both mesh and pcurve sampling. The published IFC authoring examples use
this representation scope. [IFC profile tags](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcOpenCrossProfileDef.htm)
· [Guide hosting and examples](https://github.com/RickBrice/IFC-Alignment-Geometry-Implementation-Guide/blob/main/10_Sectioned_Surfaces_and_Solids.md#1052-guide-curve-hosting-and-tag-resolution)

Guide anchors must match authored profile points within 1 mm. Accepted
rounding differences are blended between consecutive authored sections so
their positions remain exact. Contradictory anchors, duplicate matches, guides
in different frames, and detected folds, crossings or gaps produce diagnostics
with source IDs. A guide can control one point per cross section for each tag.
On a smooth directrix, an intermittent tag on an open cross profile can take a
guide on one contiguous run of at least two authored rows. The guide must match
the authored branch tips and acts only between rows carrying the tag. Outside
that span the collapsed breakline follows its retained anchor, including any
guide bound to that anchor.
Repeated tags and guides spanning disjoint runs remain outside this coverage.
There is a 128-guide limit per surface and a 4,097-sample limit per guide.
Independent guides on smooth directrices use chord-refined support and require
a monotone projection on the section normal.

An open profile backed by an indexed 2D polycurve can supply its tags in the
point list's `TagList`. The importer looks up labels by point index and matches
columns in curve traversal order. Segment endpoints shared by adjacent
segments occupy one column. Unused points contribute no columns, and empty
labels leave their columns unbound. All sections must have the same traversed
tag order, including unbound columns. An `IfcArcIndex` retains its analytic
curvature and assigns its through-point a column at the point's actual native
parameter. Over a guided curved span, the surface interpolates the guide
displacements between adjacent tagged points on top of that analytic shape.
Guided arcs at a sharp miter and branching indexed tag runs are outside this
coverage. [IFC point-list tags](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcCartesianPointList2D.htm)

Straight planar miters support guides where the authored profiles have fixed
lateral columns. The guide follows the planes of the ruled miter rows,
including the shared half-angle row at the bend. An independent guide can use
an unoffset polyline or a line-only indexed polycurve. It must reach each row
and cross its plane exactly once in consistent order. The importer checks
changes in the intersection at guide vertices and preserves those vertices as
sampling stations for the mesh and pcurves. Guides on the same horizontal
polyline directrix can use offsets that remain in the ruled section planes.
Changing lateral columns and explicit section directions at the corner remain
outside this planar guide construction.

Enable `CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` and run
`ctest -R '^ifc_sectioned_guides_gpu_regression$' --output-on-failure` in the
configured build directory. The fixed-camera fixture checks indexed guide
tags in scrambled storage order, a quarter-arc profile and the shared bend
edge of a guided planar miter. It runs Forward and Deferred raster with TAA
off and on.
Run `ifc_sectioned_tags_gpu_regression` with the same switch to check the
authored breakline tips, a temporarily guided lane and the shoulders in those modes.

Sharp joins use the half-angle miter prescribed by the
[sectioned-surface construction rules](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSectionedSurface.htm).
The shared row lies in the plane whose normal bisects the incident tangents.
The importer projects each incident profile to that plane along its tangent.
Compatible spatial joins preserve the shared row exactly and seed it in the
native mesh and pcurve domains, including where a branch tip meets a join.
Straight incident spans apply the correction linearly by arc length up to
the neighboring authored section or join. Constant frames with affine profile
variation produce ruled spans. Curved spans use a cubic correction over four
times the maximum offset displacement, with a small minimum based on the
chord tolerance. The native sweep resumes beyond that interval. Authored
sections limit the correction's extent.

The maximum half-angle extension factor is ten. The converter checks regular
incident tangents, authored frame compatibility, curvature, progress along
the join, and detected crossings of the directrix and the mesh. Both directrix
and triangle validation have eight-million pair budgets. The station and
profile axes are each limited to 4,097 samples. Incompatible or strongly
twisted authored join frames remain outside coverage.

Curved and spatial miters can follow independent guides on continuous profile
tags or on one contiguous authored run. Each tag uses its transported transverse
plane through the authored or mitered point. The guide must agree with the
authored stations and shared half-angle rows within its run, including any
branch tip at a join. Outside that run the collapsed column follows its retained
anchor. The mesh and pcurve retain the guide stations.

A guide on the directrix can use authored offsets when the curve stays
continuous through the join and its points lie on the transported section
planes. This includes a vertical crown through a horizontal bend and a lateral
offset that returns to the shared row before a bend. The importer compares
incident points at sharp joins, so a grade break that gives conflicting
positions for the same offset produces a source guide diagnostic.
[IFC offset directions](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcPointByDistanceExpression.htm)

Run `ifc_sectioned_miters_gpu_regression` with
`CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` to compare the spatial miter and the
curved miter with an independent outer rail and a shared-basis crown in Forward
and Deferred raster with TAA off and on.

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

The curve/surface/sweep/profile/B-rep increments pass all 138 importer/core cases with the optional
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
Spline seam regressions check U-closed and V-closed rational cylinders and
doubly periodic rational tori with non-unit knot domains, opposite face bounds
and authored pcurves. The cylinders also exercise bare 3D rims between authored
seam lifts. Independent volume, normal and exact-edge checks cover
complementary winding bands on the torus. A monotone band with a blocked first
cut anchor checks analytic area, exact rail edges and paired cut edges.
Conflicting lifts and periodic images
reject without leaving mesh buffers. Sharp-knot cases check that matching and
unequally sampled rings keep identical cut edges and one-sided normals.
Detached trim cases check both periodic axes, a bare outer bound or hole, and
the exact shared boundary and area of a separately styled patch.
Use
`CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1` with CTest's
`ifc_periodic_spline_seams_gpu_regression` to verify depth, source colors and
the torus bore in forward/deferred with TAA off/on.
Curve style regressions check predefined and explicit dash lengths across metre
and millimetre imports, metric widths, reflected mapped placements, layer
inheritance and through-corner phase. With the same opt-in, run
`ifc_curve_style_gpu_regression` to compare projected stroke widths, direct
styles and inherited styles on a shared curve, and verify that viewport
selection follows visible dashes.
Sectioned branch regressions check split/merge and multiway source corners,
zero-width branch tips and constant collapsed strips,
metre/millimetre stations, sloped crowns and sampled triangle chord error,
curved annular area, reversed/rotated miters and combined branches at miters.
Exact-position edge uses, Euler count and a single degree-two boundary loop
independently check mesh connectivity. Spatial sharp-join regressions also
check shared rows, pcurves and analytic ribbon areas across changes of slope
and direction. An arc-to-line case checks the native centerline and radius
away from the joint. Reordered tags, unsafe angles, folded or intersecting
patches and incompatible tangent frames reject atomically.
Derived-profile regressions independently check placement/transform order,
axis defaults and reflected senses, nested/metre/millimetre bounds, signed
extrusion winding, analytic volumes and scaled circle/indexed-arc/spline chord
error. Tests also cover surface parameters and inverses, inherited section tags,
mirrored miter orientation, inner-wall normals and solid Boolean cuts through
derived structural/void profiles. Invalid and recursive profile chains reject
atomically.
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

Four derived-profile captures display a mirrored structural Boolean cut, a
mirrored hollow ellipse, a mirrored sectioned miter, a derived sectioned surface,
two ellipse pcurves and a floor. All seven products import completely in
deferred/forward with TAA off/on at 1280x720, MSAA 1 and validation enabled,
without VUID/synchronization hazards. Camera position is `(29,31,44)`, target
`(0,5,0)`, directional/environment intensities are `2`/`0.2` and exposure
is `0.3`. Images and reports are under `out/ifc-review/derived-profiles/`.
This gallery exposed a forward lighting culling mismatch: the depth pass
rendered double-sided back faces while the lighting pass culled them. Forward
opaque lighting now uses matching single-sided, reflected and double-sided
pipelines for both scene and BIM draws. The opt-in
`forward_culling_gpu_regression` checks each visible material and hidden back
face separately in glTF/IFCX, deferred/forward and TAA off/on captures. Enable it
with `CONTAINER_RUN_GPU_FORWARD_CULLING=1` and run the named CTest; Pillow is
required.

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

$env:CONTAINER_RUN_GPU_IFC_AREA_PROFILES = '1'
$env:CONTAINER_RUN_GPU_FORWARD_CULLING = '1'
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^(ifc_.*_gpu_regression|forward_culling_gpu_regression)$'

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

Scene Controls discovers both IFCX layers and native STEP IFC examples.
Native files have a `(STEP)` suffix in the sample picker, so the two formats
have distinct labels and ImGui IDs. Discovery includes the buildingSMART IFC
sample repository, rather than requiring tessellation/extrusion token hints.
Failed loads retain the detailed importer error in the UI and renderer log.
The previous scene is restored when an auxiliary load fails.
GitHub archive downloads can contain Git LFS pointer files. Asset generation
materializes IFC/IFCX payloads at the archive's recorded Git revision and verifies
their declared SHA-256 and byte size before replacing each pointer. Cached real
model files require no further LFS download. A failed or corrupt download keeps
the pointer intact and fails the asset target, so packaging cannot silently
include placeholder models.
BIM loads use an exterior camera overview. The interior hall heuristic remains
available for primary glTF scenes such as Sponza; applying it to a thin IFC wall
could start the camera inside the model's bounds and show a blank-looking view.

The native coverage review and its independent geometry and rendering criteria
are tracked in [#67](https://github.com/semihguresci/VulkanSceneRenderer/issues/67).

Ambiguous tag transitions, incompatible sharp joins, uncertified singular
charts and polar trims with separate meridians or holes meeting an authored seam
remain outside native coverage. Swept surface limits include standalone
revolutions with cusps or axis crossings and multi-chart boundaries without
enough 3D geometry to select their chart. IFC swept surfaces use
`CURVE` profiles. An arbitrary `AREA` profile with voids can be used with an
`IfcExtrudedAreaSolid`.
[Swept surface](https://standards.buildingsmart.org/IFC/RELEASE/IFC4_3/HTML/lexical/IfcSweptSurface.htm)
Swept-area solids cover the IFC4 parameterized profile families listed above.
IFC4 replaced the legacy crane-rail types with arbitrary closed or asymmetric I
profiles. [IFC4 profile changes](https://standards.buildingsmart.org/IFC/RELEASE/IFC4/FINAL/HTML/schema/ifcprofileresource/lexical/ifcparameterizedprofiledef.htm)

Periodic trims without a valid connector at shared samples, ambiguous chart
lifts and inputs that exceed the search budgets produce diagnostics. Each
omitted representation is reported.

The Hello Wall body surfaces agree across STEP/IFCX at sampled triangle corners,
edge midpoints and centroids in both directions within 0.01 mm, allowing
different triangulation. Tekla overall bounds agree within 0.01 mm. Representative
shared wall, column, beam, slab and footing bodies pass bidirectional corner and
centroid surface checks with a 2 mm combined chord-error budget. IFCX has no
reinforcing-bar meshes in this export; swept disks instead have independent
analytic volume and normal tests. These checks do not establish equivalence for
every product or material. The sampled recovery requirements from
[#61](https://github.com/semihguresci/VulkanSceneRenderer/issues/61) are implemented;
further native IFC coverage is tracked separately in #67.
