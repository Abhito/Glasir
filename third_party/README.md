# Third-party source in this repo

Only files whose licences allow it are kept here, unmodified, each with its
licence. Binaries never go in the repo; Glasir downloads them on the user's PC (see
`docs/PLAN.md` and `THIRD-PARTY-NOTICES.md`).

| Folder | What | Version / source | Licence |
|---|---|---|---|
| `reshade/include/` | ReShade add-on API headers | crosire/reshade tag `v6.8.0` (commit `18deaa52de0c`), `include/` | BSD-3-Clause, `reshade/LICENSE.md` |
| `mpv/include/mpv/` | mpv client API header (`client.h`) | mpv-player/mpv tag `v0.41.0` (commit `2c219aa822df`), `include/mpv/client.h` | ISC, text at the top of the file |

## SHA-256

```
cf3cd150920ea310b4b1e2e4f11ab8d7221a9fc18755f4891a026563311b524c  reshade/include/reshade.hpp
8d83ec8a620c48da7a20d975bd27b3345b24d3f0d5b78ae4c4fe633d61b804eb  reshade/include/reshade_api.hpp
a716c44b6fbb064dbbb0826687728f21aa71ed45e13564e4dc1cdf36da5bdfa0  reshade/include/reshade_api_device.hpp
68caa17fef712043e810ab88770c15af009aa3c7693e5c115ddc324f2d0809a7  reshade/include/reshade_api_format.hpp
5bda8e07260c2c60f0fd18a78b11782aed8f69e5d24b5757874614caafdf416f  reshade/include/reshade_api_pipeline.hpp
32cdafe6d783813f3ac72ad43e426638d8d0fef56bcb558fad218a14cfb75ee3  reshade/include/reshade_api_resource.hpp
2717f56e2bd89af6d708c9c316353d6581c5a41d4526629fadba51d13be57a89  reshade/include/reshade_events.hpp
ed914b12fbc41ed038287b0163c0bc934772afcff2c22392044c6008adc5ae79  reshade/include/reshade_overlay.hpp
237ded5b8344f820113efab1e65e91e1f159d9202c5b4856606a0590d3ffdab0  reshade/LICENSE.md
3c073236a09cb456c6e80587d5523c8771c32fb22ca014e0f99cc3d43905b75c  mpv/include/mpv/client.h
```

To update: download the files from the new upstream tag, replace them here,
update the table and the hashes, and note it in the commit message.
`reshade_overlay.hpp` needs Dear ImGui and is not used yet.
