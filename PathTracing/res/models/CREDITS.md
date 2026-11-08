# Crediti dei modelli

I modelli grandi non stanno nel repo: li scarica `tools/fetch_assets.sh` (o `.ps1`), ognuno fissato
a una versione e verificato con SHA-256. Ogni cartella contiene la licenza originale.

| Cartella | Modello | Autore | Licenza | Fonte | Scena |
|---|---|---|---|---|---|
| `Sponza/` | Crytek Sponza | Frank Meinl, Crytek | CC BY 3.0 | [jimmiebergmann/Sponza](https://github.com/jimmiebergmann/Sponza) | 08 |
| `DamagedHelmet/` | Damaged Helmet | ctxwing (glTF), da un modello di theblueturtle_ | CC BY 4.0 e CC BY-NC 4.0 (il modello originale: niente uso commerciale) | [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet) | 09 |
| `MetalRoughSpheres/` | Metal-Rough Spheres | Ed Mackey, Analytical Graphics | CC BY 4.0 | [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/MetalRoughSpheres) | 10 |
| `BreakfastRoom/` | Breakfast Room | Wig42 (Blend Swap), conversione di Morgan McGuire | CC BY 3.0 | [McGuire Computer Graphics Archive](https://casual-effects.com/data/) | 11 |
| `Bistro/` | Amazon Lumberyard Bistro | Amazon Lumberyard, materiali rivisti da Morgan McGuire | CC BY 4.0 | [McGuire Computer Graphics Archive](https://casual-effects.com/data/), [NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) | 12 |
| `ZeroDay/` | Zero-Day, Measure Seven | Mike Winkelmann (Beeple), Kate Anderson | CC BY 4.0 | [NVIDIA ORCA](https://developer.nvidia.com/orca/beeple-zero-day), convertito in glTF da `tools/zero_day` | 13 |

Le modifiche fatte qui: Zero-Day passa da FBX con texture DDS a glTF con PNG/JPEG (texture
ridotte a 2048 px, normal map da DirectX a OpenGL, mesh duplicate rese istanze); le scene integrate
aggiungono inquadratura, sole e impostazioni di render.
