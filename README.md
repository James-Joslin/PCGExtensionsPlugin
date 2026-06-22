# Unreal Engine 5 Custom PCG Extensions Framework

A high-performance, optimized C++ plugin module extending Unreal Engine 5's Procedural Content Generation (PCG) framework. This plugin consolidates heavy multi-node Blueprint workflows into standalone, native execution blocks—significantly reducing evaluation overhead, graph complexity, and metadata pressure.

## 🛠 Features & Node Matrix

### 1. Advanced Structural Generation
* **Dynamic Mesh Embed (`UPCGDynamicMeshEmbedSettings`)**
    * *Purpose:* Replaces complex logic checking surface normals and bounding box support structures.
    * *Mechanics:* Computes explicit mathematical contact points based on localized object bounds and surface normal alignments, automatically calculating optimal depth offsets to structurally anchor rocks or assets deep into uneven terrain.
* **Crest Cap Detector (`UPCGCrestCapDetectorSettings`)**
    * *Purpose:* Works as a direct architectural companion to the Dynamic Mesh Embed system for advanced geological profiling.
    * *Mechanics:* Designed for scenarios where rocks are embedded into steep, sloped cliffs. It casts vertical search rays from the transformed positions of those embedded shapes to find where their upper geometric extents break out into flat ground or open sky. It isolates these points, resets their normal orientation to match the local top surface, and prepares them for an independent "cap mesh" pass to cleanly cover and seal protruding sections.
* **Rock Formation Generator (`UPCGRockFormationGeneratorSettings`)**
    * *Purpose:* Generates complex, multi-tier rock formations (Foundation, Mid-Tier, and Cap Rocks) in a single procedural loop.
    * *Mechanics:* Dynamically evaluates bottom-face vertices to calculate overhang thresholds, placing responsive support/fill assets directly matching the layer they anchor from.

### 2. High-Density Scatter & Foliage Pipelines
* **Scatter Around Points (`UPCGScatterAroundPointsSettings`)**
    * *Purpose:* Flattens heavy clusters of native nodes (Copy Points, Distance, Transforms, Filters) into a single disk-distribution pass.
    * *Mechanics:* Features built-in mathematical distance falloffs, landscape raycasting, slope-band extraction, and deterministic native self-pruning. Supports an explicit raw pass-through mode optimized for feeding directly into the `Dynamic Mesh Embed` system.
* **Tiered Vegetation Scatter (`UPCGTieredVegetationScatterSettings`)**
    * *Purpose:* Handles per-point validation loops for complex ecosystem hierarchies.
    * *Mechanics:* Implements native single-layer Perlin noise clustering, weighted average/max biome attribute blending, obstacle footprint rejection, and structural understory/companion ring emission.
* **Ground Cover Scatter (`UPCGGroundCoverScatterSettings`)**
    * *Purpose:* Ultra-high throughput procedural point generation for grass, debris, and low-lying ground assets.
    * *Mechanics:* Leverages a deterministic cellular grid spatial optimization scheme for fast evaluation loops without high memory overhead.

---

## 📂 Project Architecture

```text
Plugins/PCGExtensions/
├── Source/
│   └── PCGExtensions/
│       ├── Public/
│       │   ├── PCGCrestCapDetector.h
│       │   ├── PCGDynamicMeshEmbed.h
│       │   ├── PCGGroundCoverScatter.h
│       │   ├── PCGRockFormationGenerator.h
│       │   ├── PCGScatterAroundPoints.h
│       │   └── PCGTieredVegetationScatter.h
│       └── Private/
│           ├── PCGCrestCapDetector.cpp
│           ├── PCGDynamicMeshEmbed.cpp
│           ├── PCGGroundCoverScatter.cpp
│           ├── PCGRockFormationGenerator.cpp
│           ├── PCGScatterAroundPoints.cpp
│           └── PCGTieredVegetationScatter.cpp
├── PCGExtensions.uplugin
└── README.md
```

## 🚀 Getting Started

### Prerequisites
* **Unreal Engine:** 5.3+
* **Module Dependencies:** Requires `PCG` explicitly referenced in your project's or client module's `Build.cs` configurations:
    ```csharp
    PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "PCG" });
    ```

### Compilation & Integration
1. Close the Unreal Editor.
2. Clone this repository directly into your project's `Plugins/` subdirectory.
3. Right-click your project's `.uproject` file and select **Generate Visual Studio project files**.
4. Open the resulting `.sln` workspace and build your solution target configuration under **Development Editor**.
5. Launch the project, verify that the **PCG Extensions Plugin** is active under *Edit -> Plugins*, and search for the native nodes directly within any PCG Graph interface.