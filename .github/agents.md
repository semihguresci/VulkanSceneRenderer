# Agents

Instructions for AI coding agents working on this repository.

## Required Reading

Before making changes to rendering code, shaders, or coordinate/math-related logic, agents **must** consult the following document:

- **[Coordinate & Rendering Conventions](../docs/coordinate-conventions.md)** — Defines the coordinate system, projection, viewport, depth, winding, G-buffer encoding, normal map, and matrix upload conventions for the entire engine.

## Key Rules

### Coordinate System
- The engine uses a **right-handed** coordinate system with **+Y up**, **+X right**, **−Z camera forward** (glTF native).

### Vulkan Y-Flip
- The Y-flip is performed via a **negative-height viewport**, not in the projection matrix.
- **Never** add `proj[1][1] *= -1` or any matrix-level Y-flip.
- Scene passes use `viewport.y = height`, `viewport.height = -height`.
- Shadow passes use a standard positive-height viewport.

### Reverse-Z Depth
- Near plane maps to **1.0**, far plane maps to **0.0**.
- Depth buffer is cleared to **0.0**.
- Depth compare is `VK_COMPARE_OP_GREATER_OR_EQUAL`.

### NDC ↔ UV Conversion
- Converting between NDC and UV coordinates requires **negating Y** due to the negative-height viewport.
- Fullscreen passes should derive UV from `SV_Position` to avoid manual conversion.

### Winding & Culling
- Front face: `VK_FRONT_FACE_COUNTER_CLOCKWISE`.
- Scene cull: `VK_CULL_MODE_BACK_BIT`.
- Shadow cull: `VK_CULL_MODE_BACK_BIT`.

### Matrix Convention
- GLM column-major matrices are uploaded directly to Slang shaders — **no transpose**.

### Shader Language
- Shaders use **Slang** (`.slang` files) with column-major matrix convention.

## Project Standards

- Use **modern Vulkan** (1.1+) and **modern computer graphics** practices.
- Use the **Visual Studio Developer Console** environment for terminal and build commands.
- Build system: **CMake** with **Ninja** generator, **C++23** standard.

