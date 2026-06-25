# PCG Extensions Plugin — Unreal Engine 5.7

## 🎬 Demo

![Rock formation generation demo](images/preview-gif.gif)

A native C++ plugin that extends Unreal Engine's Procedural Content Generation (PCG) framework with consolidated, high-performance nodes. Each node replaces a chain of built-in nodes (Surface Sampler → Normal To Density → Density Filter → Self Pruning → …) with a single execution block that handles the full pipeline internally — fewer graph nodes, less metadata overhead, faster iteration.

Built for open-world landscape population: rocks on slopes, multi-tier formations on flat ground, tiered vegetation, high-density grass, and foliage on rock surfaces.

---

## Nodes

### Dynamic Mesh Embed

Embeds rocks into terrain at the correct depth regardless of slope angle. Instead of a fixed Z offset that fails on steep surfaces, it projects the mesh's bounding box along the surface normal using a support function, so a rock on an 80° cliff face sinks into the hillside rather than sliding down it.

**Typical wiring:**

`Surface Sampler → Normal To Density → Density Filter → Dynamic Mesh Embed → Transform Points → Static Mesh Spawner`

The node assigns a weighted-random mesh from a configured set, writes the path as a `MeshPath` attribute, computes the embed offset from the mesh bounds and surface normal, and shifts the point into the surface. Feed the output into a Static Mesh Spawner set to "By Attribute" on `MeshPath`.

---

### Crest Cap Detector

Finds the flat ground above cliff-embedded rocks and places cap meshes there. Works as a companion to Dynamic Mesh Embed for slope scenarios: it takes the embedded rock positions, casts vertical rays upward to find where the cliff breaks into flat terrain, and outputs new points at those crest locations with their normals reset to match the local top surface.

**Typical wiring:**

`Dynamic Mesh Embed → Crest Cap Detector → Transform Points → Static Mesh Spawner`

In the slopes graph, the same Dynamic Mesh Embed output feeds both the slope rock spawner and the crest cap path. The cap rocks visually seal the top of cliff faces where embedded slope rocks protrude through to flat ground above.

---

### Scatter Around Points

Distributes secondary rocks (medium, small) in disk patterns around parent rock positions. Replaces a chain of Copy Points → Distance → Transform → Filter nodes with a single pass that handles radial distribution, landscape projection, slope-band filtering, and min-distance pruning internally.

**Typical wiring:**

`Parent Rock Spawner Out → Scatter Around Points → Dynamic Mesh Embed → Transform Points → Static Mesh Spawner`

Used in the slopes graph for medium and small rock tiers — each tier takes the previous tier's spawner output as parent positions and scatters child rocks around them. Includes a pass-through mode optimised for feeding directly into Dynamic Mesh Embed (skips projection, lets the embed node handle surface contact).

---

### Rock Formation Generator

Generates multi-tier rock formations (foundation clusters, mid-tier rocks, cap fills) from a single sparse input point. Replaces what would otherwise be several nested Scatter Around Points + embed chains. Internally evaluates bottom-face vertices to detect overhangs and places responsive fill meshes where gaps would be visible.

**Typical wiring:**

`Surface Sampler → Normal To Density → Density Filter → Difference (subtract slope volumes) → Rock Formation Generator → Static Mesh Spawner`

In the flat surfaces graph, the Difference node subtracts the slope rock volumes from the candidate landscape so formations only generate on open flat ground. The Formation Generator output goes straight to a By-Attribute Static Mesh Spawner — no intermediate Transform Points needed since the node handles scale/rotation internally.

---

### Tiered Vegetation Scatter

Places one vegetation tier (large trees, medium trees, or shrubs) from an input candidate point cloud. Handles landscape projection, slope-band filtering, per-biome density/scale response from weight attributes, mesh-bounds obstacle exclusion (avoids rock footprints), single-layer noise thinning, min-distance pruning, weighted mesh assignment, and optional companion output (understory plants around each placed tree).

**Typical wiring:**

`Surface Sampler → Difference (subtract rock zones) → Tiered Vegetation Scatter → Static Mesh Spawner`

The node is instanced once per tier with different settings. The `Out` pin carries the main vegetation (e.g. large trees placed with the node's own mesh assignment). The `Companions` pin carries companion ring points that reference the meshes the node assigned — feed these to a separate Transform Points → Static Mesh Spawner chain for understory plants around each tree. Rock zone exclusion is handled by feeding the Union of all rock spawner outputs into the Difference node upstream.

---

### Ground Cover Scatter

Ultra-high-throughput grass and ground-cover placement. Takes a dense candidate point cloud and filters it through a multi-layer noise stack (Perlin × Worley + fine detail + micro jitter), per-biome density response, obstacle exclusion (rock footprints), and spatial-hash min-distance pruning. Designed for point counts in the hundreds of thousands — the spatial hash keeps it linear where brute-force approaches would be O(N²).

**Typical wiring:**

`Surface Sampler → Difference (subtract exclusion zones) → Ground Cover Scatter → Static Mesh Spawner`

The Exclusion Zones input on the parent graph carries the Union of all rock spawner outputs. The Difference node subtracts these footprints from the landscape sample before the points reach Ground Cover Scatter. The node assigns grass meshes via a weighted set and writes `MeshPath` for the By-Attribute spawner.

---

### Mesh Surface Scatter

Samples the actual triangle geometry of rock meshes and places grass/foliage only on flat, wide, non-edge surfaces. Three surface-quality filters work together: normal dot-up (only near-horizontal faces), BFS edge-distance (rejects points near the boundary of walkable areas — catches narrow ridges, pointed tips, and mesh perimeter), and neighbor curvature (rejects triangles on creased or irregular geometry). Handles partially submerged rocks by multi-tracing to the landscape and rejecting samples below terrain.

**Typical wiring:**

`Rock Spawner Out (with MeshPath) → Mesh Surface Scatter → Static Mesh Spawner`

Takes the individual mesh bounds output from both rock graphs (slopes and flat surfaces). Each rock point's `MeshPath` attribute is used to load the mesh geometry, the point's transform positions the triangles in world space, and the node samples eligible surfaces with area-weighted random placement. The output carries its own `MeshPath` attribute for the grass meshes.

**Note:** Rock meshes need "Allow CPU Access" enabled in the mesh asset for runtime PCG generation. For standard editor-time PCG (no world streaming), this is not required — vertex buffers are readable unconditionally in the editor.

---

## Pipeline Overview

The master graph orchestrates these nodes across subgraphs:

```
Get Landscape Data
    ├── PCG Rocks Slopes ──────────────┐
    │   (DynamicMeshEmbed,             │
    │    CrestCapDetector,             ├── Union ──┬── PCG Tiered Foliage
    │    ScatterAroundPoints)          │            │   (TieredVegetationScatter × 3 tiers)
    ├── PCG Rocks Flat Surfaces ───────┘            │
    │   (RockFormationGenerator)                    ├── PCG Ground Cover
    │                                               │   (GroundCoverScatter)
    ├── Individual Mesh Bounds ─────────────────────┴── PCG Scatter on Mesh
    │   (per-rock MeshPath + transforms)                (MeshSurfaceScatter)
```

Rock placement runs first, outputting both exclusion zones (bounding boxes for vegetation to avoid) and individual mesh bounds (per-rock MeshPath + transform data for surface sampling). Vegetation and ground cover subtract the exclusion zones before scattering. Mesh surface scatter reads the individual mesh bounds directly.

---

## Project Structure

```
PCGExtensions/
├── Resources/
│   └── Icon128.png
├── Source/
│   └── PCGExtensions/
│       ├── Public/
│       │   ├── PCGCrestCapDetector.h
│       │   ├── PCGDynamicMeshEmbed.h
│       │   ├── PCGGroundCoverScatter.h
│       │   ├── PCGMeshSurfaceScatter.h
│       │   ├── PCGRockFormationGenerator.h
│       │   ├── PCGScatterAroundPoints.h
│       │   └── PCGTieredVegetationScatter.h
│       ├── Private/
│       │   ├── PCGCrestCapDetector.cpp
│       │   ├── PCGDynamicMeshEmbed.cpp
│       │   ├── PCGGroundCoverScatter.cpp
│       │   ├── PCGMeshSurfaceScatter.cpp
│       │   ├── PCGRockFormationGenerator.cpp
│       │   ├── PCGScatterAroundPoints.cpp
│       │   └── PCGTieredVegetationScatter.cpp
│       └── PCGExtensions.Build.cs
├── PCGExtensions.uplugin
├── LICENSE
└── README.md
```

## Installation

**Requirements:** Unreal Engine 5.7

1. Clone into your project's `Plugins/` directory:
   ```
   cd YourProject/Plugins
   git clone https://github.com/James-Joslin/PCGExtensionsPlugin.git PCGExtensions
   ```
2. Regenerate project files (right-click `.uproject` → Generate Visual Studio project files).
3. Build and launch. The plugin's `Build.cs` already declares the PCG module dependency — no changes to your project's Build.cs are needed.
4. Verify under Edit → Plugins that **PCG Extensions** is enabled, then search for the nodes by name in any PCG Graph.

## License

MIT License — see [LICENSE](LICENSE).
