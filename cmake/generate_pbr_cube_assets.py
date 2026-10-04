#!/usr/bin/env python3
"""
Generate PBR cube glTF assets and supporting textures at build time.

This script avoids embedding pre-generated binary assets in the source tree by
reconstructing the textured cube model and writing it into the build output
folder. It also optionally copies additional static models so they remain
available alongside the generated assets.
"""
from __future__ import annotations

import argparse
import base64
import json
import pathlib
import shutil
from textwrap import dedent

PNG_TEXTURES = {
    "cube_baseColor.png": "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAG0lEQVR4nGO44+b2/0SAxn+GLRpR/59VRP0HAFMyCZNCbUCLAAAAAElFTkSuQmCC",
    "cube_emissive.png": "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAG0lEQVR4nGNg0DjxX27Bnf8MDBUL/gec+P8fAEtECbZb4e95AAAAAElFTkSuQmCC",
    "cube_metallicRoughness.png": "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEElEQVR4nGNgcDjzH4xhDABCjAgtI1mSjwAAAABJRU5ErkJggg==",
    "cube_normal.png": "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAGElEQVR4nGNoaPj/v2HL//8MW0CMhjv/AWucDD7KbFM2AAAAAElFTkSuQmCC",
    "cube_occlusion.png": "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR4nGP4z8Dw/wQQMywAEneAGABBewdAlMWopQAAAABJRU5ErkJggg==",
}

PBR_CUBE_GLTF = dedent(
    """
    {
      "asset": {
        "version": "2.0",
        "generator": "Custom Python script"
      },
      "buffers": [
        {
          "byteLength": 840,
          "uri": "data:application/octet-stream;base64,AACAPwAAgL8AAIC/AACAPwAAgD8AAIC/AACAPwAAgD8AAIA/AACAPwAAgL8AAIA/AACAvwAAgL8AAIA/AACAvwAAgD8AAIA/AACAvwAAgD8AAIC/AACAvwAAgL8AAIC/AACAvwAAgD8AAIC/AACAvwAAgD8AAIA/AACAPwAAgD8AAIA/AACAPwAAgD8AAIC/AACAvwAAgL8AAIA/AACAvwAAgL8AAIC/AACAPwAAgL8AAIC/AACAPwAAgL8AAIA/AACAvwAAgL8AAIA/AACAPwAAgL8AAIA/AACAPwAAgD8AAIA/AACAvwAAgD8AAIA/AACAPwAAgL8AAIC/AACAvwAAgL8AAIC/AACAvwAAgD8AAIC/AACAPwAAgD8AAIC/AACAPwAAAAAAAAAAAACAPwAAAAAAAAAAAACAPwAAAAAAAAAAAACAPwAAAAAAAAAAAACAvwAAAAAAAAAAAACAvwAAAAAAAAAAAACAvwAAAAAAAAAAAACAvwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAgL8AAAAAAAAAAAAAgL8AAAAAAAAAAAAAgL8AAAAAAAAAAAAAgL8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIC/AAAAAAAAAAAAAIC/AAAAAAAAAAAAAIC/AAAAAAAAAAAAAIC/AACAPwAAAAAAAIA/AACAPwAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAgD8AAIA/AAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAACAPwAAgD8AAAAAAACAPwAAAAAAAAAAAACAPwAAAAAAAIA/AACAPwAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAgD8AAIA/AAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAACAPwAAgD8AAAAAAACAPwAAAAAAAAAAAAABAAIAAAACAAMABAAFAAYABAAGAAcACAAJAAoACAAKAAsADAANAA4ADAAOAA8AEAARABIAEAASABMAFAAVABYAFAAWABcA"
        }
      ],
      "bufferViews": [
        {
          "buffer": 0,
          "byteOffset": 0,
          "byteLength": 288
        },
        {
          "buffer": 0,
          "byteOffset": 288,
          "byteLength": 288
        },
        {
          "buffer": 0,
          "byteOffset": 576,
          "byteLength": 192
        },
        {
          "buffer": 0,
          "byteOffset": 768,
          "byteLength": 72,
          "target": 34963
        }
      ],
      "accessors": [
        {
          "bufferView": 0,
          "componentType": 5126,
          "count": 24,
          "type": "VEC3",
          "min": [
            -1,
            -1,
            -1
          ],
          "max": [
            1,
            1,
            1
          ]
        },
        {
          "bufferView": 1,
          "componentType": 5126,
          "count": 24,
          "type": "VEC3"
        },
        {
          "bufferView": 2,
          "componentType": 5126,
          "count": 24,
          "type": "VEC2"
        },
        {
          "bufferView": 3,
          "componentType": 5123,
          "count": 36,
          "type": "SCALAR"
        }
      ],
      "images": [
        {
          "uri": "cube_baseColor.png"
        },
        {
          "uri": "cube_metallicRoughness.png"
        },
        {
          "uri": "cube_normal.png"
        },
        {
          "uri": "cube_occlusion.png"
        },
        {
          "uri": "cube_emissive.png"
        }
      ],
      "textures": [
        {
          "source": 0
        },
        {
          "source": 1
        },
        {
          "source": 2
        },
        {
          "source": 3
        },
        {
          "source": 4
        }
      ],
      "materials": [
        {
          "name": "CubePBR",
          "pbrMetallicRoughness": {
            "baseColorFactor": [
              0.85,
              0.35,
              0.25,
              1.0
            ],
            "baseColorTexture": {
              "index": 0
            },
            "metallicFactor": 0.8,
            "roughnessFactor": 0.25,
            "metallicRoughnessTexture": {
              "index": 1
            }
          },
          "normalTexture": {
            "index": 2,
            "scale": 1.0
          },
          "occlusionTexture": {
            "index": 3,
            "strength": 1.0
          },
          "emissiveFactor": [
            0.0,
            0.25,
            0.8
          ],
          "emissiveTexture": {
            "index": 4
          }
        }
      ],
      "meshes": [
        {
          "primitives": [
            {
              "attributes": {
                "POSITION": 0,
                "NORMAL": 1,
                "TEXCOORD_0": 2
              },
              "indices": 3,
              "material": 0
            }
          ]
        }
      ],
      "nodes": [
        {
          "mesh": 0
        }
      ],
      "scenes": [
        {
          "nodes": [
            0
          ]
        }
      ],
      "scene": 0
    }
    """
).strip() + "\n"


def write_pngs(output_dir: pathlib.Path) -> None:
    for filename, data_b64 in PNG_TEXTURES.items():
        target = output_dir / filename
        target.write_bytes(base64.b64decode(data_b64))


def write_gltf(output_dir: pathlib.Path) -> None:
    buffer = json.loads(PBR_CUBE_GLTF)["buffers"][0]
    payload = base64.b64decode(buffer["uri"].split(",", 1)[1], validate=True)
    if len(payload) != buffer["byteLength"]:
        raise ValueError("Generated PBR cube buffer does not match its declared byteLength")
    target = output_dir / "basic_cube.gltf"
    target.write_text(PBR_CUBE_GLTF)


def copy_static_models(static_models: list[str], output_dir: pathlib.Path) -> None:
    for model_path in static_models:
        source = pathlib.Path(model_path)
        if not source.exists():
            raise FileNotFoundError(f"Static model not found: {source}")
        destination = output_dir / source.name
        shutil.copy2(source, destination)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate PBR cube glTF assets.")
    parser.add_argument(
        "--output-dir",
        type=pathlib.Path,
        required=True,
        help="Where to write the generated assets.",
    )
    parser.add_argument(
        "--static-model",
        action="append",
        default=[],
        help="Additional glTF files to copy alongside generated assets.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    write_pngs(output_dir)
    write_gltf(output_dir)
    copy_static_models(args.static_model, output_dir)


if __name__ == "__main__":
    main()
