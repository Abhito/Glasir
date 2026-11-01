# Third-party source in this repo

Only files whose licences allow it are kept here, unmodified, each with its
licence. Binaries never go in the repo; Glasir downloads them on the user's PC (see
`docs/PLAN.md` and `THIRD-PARTY-NOTICES.md`).

| Folder | What | Version / source | Licence |
|---|---|---|---|
| `reshade/include/` | ReShade add-on API headers | crosire/reshade tag `v6.8.0` (commit `18deaa52de0c`), `include/` | BSD-3-Clause, `reshade/LICENSE.md` |
| `mpv/include/mpv/` | mpv client API header (`client.h`) | mpv-player/mpv tag `v0.41.0` (commit `2c219aa822df`), `include/mpv/client.h` | ISC, text at the top of the file |
| `imgui/` | Dear ImGui: core, DX11 and Win32 backends (the in-game menu, the launcher) | ocornut/imgui tag `v1.92.5` (commit `6d910d5487d1`), the root sources and `backends/imgui_impl_{dx11,win32}` | MIT, `imgui/LICENSE.txt` |

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
c80c5789748d955c4a650562baa0d750494e2c7128c2ca66aaebe2e3c4e198bf  imgui/LICENSE.txt
6e5687893594ebfaf8569280cfea83d025904a8a4e23bec073086f6c5bc8f7fb  imgui/imconfig.h
86f0fe3e1ebb1f74d1594f01d5b30c14d0ac096caf27f09903174bca800cc8e1  imgui/imgui.cpp
768cf03c079e1ce198d207c7a3cfbcf2bdebe84d930829f73e09ae340daa265a  imgui/imgui.h
2aa2312c2e1aa1eee51a8d86322ce1a7d640ff79e3a63598ce329f0e5e74cc17  imgui/imgui_draw.cpp
cdc85290bed18d70b2875aa12dbc4b58e6c4f8fac37e44cb7889ccd002cb4f91  imgui/imgui_internal.h
c39e859e844406098dbc5e1387fcdac328878ad1f9360aa44677374e5e2806fd  imgui/imgui_tables.cpp
1a97693653262484ebc16ef18449cbbf5fe1291e0574f11288ac6013ae44b7f8  imgui/imgui_widgets.cpp
2efa3d5f7d003c19743b15155dad9f46d9f9fc783a18893d703b431d3c990972  imgui/imstb_rectpack.h
a985f5fa0ed97353d493b497961e9eef52082edcd045cf6954b69990ec9d0741  imgui/imstb_textedit.h
88f0a25e27f5eefd6ec50c42fc5fe3026aa8170603efa08aa18d622db8660923  imgui/imstb_truetype.h
b167fc1d43362fae5f4b6fe392b2a692dfd747cff76e1596ee1acba52c5538b7  imgui/backends/imgui_impl_dx11.cpp
2f8c0eeff769d7829f8293e5e404743d91bd78d9336877961bc627d3a2831bb6  imgui/backends/imgui_impl_dx11.h
d01d3bc361682aad99f79d59fccdbf03d325b2b3e124d93dfd63168d362adf2c  imgui/backends/imgui_impl_win32.cpp
7e720e7fc54e348152cf8b29145f3d291da5e9026176a36d08f5b46563f7f672  imgui/backends/imgui_impl_win32.h
```

To update: download the files from the new upstream tag, replace them here,
update the table and the hashes, and note it in the commit message.
`reshade_overlay.hpp` needs Dear ImGui and is not used yet.
