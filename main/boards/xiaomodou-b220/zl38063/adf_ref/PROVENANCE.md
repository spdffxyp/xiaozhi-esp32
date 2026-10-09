# adf_ref — provenance

Vendored reference copy of Microsemi/Espressif ZL38063 host-side sources.

- Upstream: `espressif/esp-adf`, directory `components/audio_hal/driver/zl38063/`
- Ref fetched: **`release/v2.x`** (retrieved 2026-10-07, via `api.github.com` contents API, base64 decoded byte-for-byte)
- Directory listing blob/tree SHAs at that ref:
  - `api_lib` tree `d74d69f4e704f932e70f184e37806515d588b252`
  - `example_apps` tree `567e091fae18775382d0fb388bd8d23c42f7e023`
  - `zl38063.c` blob `d1d6ad065860240e5310518e047caa979e516ad6`
  - `zl38063.h` blob `3d981770308f8f627d3e267bdb663f67d8d1aafc`
  - `LICENSE.txt` blob `ef34e5074e3b5275c918af9a134afe7221fde135`
  - `Readme_twolf.txt` blob `1f4682d3a1c72991eba06f6fff1e462e8eb4c750`
  - `api_lib/vprocTwolf_access.c` `d9cadcb24bd4e46891027d7f5b3c7665f6c8c275`
  - `api_lib/vprocTwolf_access.h` `d08d163a0ecb4c362ccf77b39ae8a1f6b8ca58fe`
  - `api_lib/vproc_common.c` `069edc39a818c4c0726fb1204fd5a29bd1f8a1fb`
  - `api_lib/vproc_common.h` `ea8d2cfad573233564d3f2800decd4f7c7745b9f`
  - `api_lib/vproc_data_types.h` `e034179aeab7cf9897ebb25c269b5f1d18ce7e60`
  - `example_apps/tw_spi_access.c` `8f8c54b64fc7a7907e16e609b697180e1b7d4536`
  - `example_apps/tw_spi_access.h` `162bf140d4b9e13996a4dc0405082765b4cbdb66`
  - `example_apps/tw_hal_verify.c` `1c1f9554c4b378cd6c26e672c2801f82b1698d5d`
  - `example_apps/tw_ldcfg.c` `b72aa4694ab4ea2d91fbec3d35de6f18f3403696`
  - `example_apps/tw_ldfw.c` `f5d99bb5526335719fa7f4967700b12d0eb6f48f`
  - `example_apps/tw_ldfwcfg.c` `f4e24705d1e2c59a4bdd2299b256a356e0725481`

## Fetch status: complete — no failures, therefore no MISSING.md

All 15 requested files were retrieved unmodified (original licence headers intact).
SHA-256 of the local copies:

| path | bytes | sha256 |
| --- | --- | --- |
| LICENSE.txt | 1074 | d8023d14377b7e734064165e0ddf5961d0bfc1ab037bc63ad682cf295369964d |
| Readme_twolf.txt | 1764 | e92a9013a8481cc52c49e865935b75feb94ac6e7bfb54fd63eca32376f486513 |
| zl38063.c | 5529 | 8843f6c7736b6523099f68f8d9fccc8da5926c444738066da9275b8417efd4ae |
| zl38063.h | 3245 | fa89fa16c0ed5ee75e023eccaceed465aad640b204e608ed46e36b75aad0ed5e |
| api_lib/vprocTwolf_access.c | 38491 | f9390f04e82678da8e5cac572e7080a7a2204975f3074476e69b672b0c743acd |
| api_lib/vprocTwolf_access.h | 3186 | a2c0dac1f32a1f14d3a073dcb103d4c743b358c092b499c9b7d6efd714aa3307 |
| api_lib/vproc_common.c | 4804 | 7ade7517b15c33b612c42b3083b2379deffdf65b648af72e417912c9cd3b4148 |
| api_lib/vproc_common.h | 3826 | 5d12c450e95b9438c11608ef911f96a39be6c6e3c14e4439603a2a2b77e96e86 |
| api_lib/vproc_data_types.h | 4481 | 04a7ba690bf36480b6663da98e598359102320b7d5edf1c5d17b5b9fa7a9afdf |
| example_apps/tw_spi_access.c | 17493 | dddce4aba2d1c9d6e91c275e93175160a47b1076481b2cf2de9734f2b07f0001 |
| example_apps/tw_spi_access.h | 168 | 3fa908745205f85575e206c16cb5f9c7ff972ded7689755158fa6c9900cdd64c |
| example_apps/tw_hal_verify.c | 10618 | b25aed7a820e8a5b6bd28a5fa96196a34da376e71f3ebc984443c525af0edaeb |
| example_apps/tw_ldcfg.c | 5160 | 1fd7aaff149c185a574266a7ff5fc77f691c8ea3c4e1c473415660528eb745a6 |
| example_apps/tw_ldfw.c | 3846 | a2444f297e692c69229a2980746f84259073172b8087dde9238c0ae4e16c5096 |
| example_apps/tw_ldfwcfg.c | 7156 | 585ca5112d399bd72611cc3fedfc5289b76c77294256722d6d4ec70ed6ad43c7 |

## Deliberately not fetched

- `firmware/zl38063_firmware.h`, `firmware/zl38063_config.h` and the prebuilt
  `firmware/libfirmware.a` — licence-gated binary firmware image, not needed here.

`components/audio_hal/driver/zl38063/` itself contains **no** `CMakeLists.txt`; the files are
compiled by `components/audio_hal/CMakeLists.txt` on `release/v2.x`:

```
./driver/zl38063/zl38063.c
./driver/zl38063/api_lib/vprocTwolf_access.c
./driver/zl38063/api_lib/vproc_common.c
./driver/zl38063/example_apps/tw_hal_verify.c
./driver/zl38063/example_apps/tw_ldcfg.c
./driver/zl38063/example_apps/tw_ldfw.c
./driver/zl38063/example_apps/tw_ldfwcfg.c
./driver/zl38063/example_apps/tw_spi_access.c)
target_link_libraries(${COMPONENT_TARGET} INTERFACE "-L${CMAKE_CURRENT_LIST_DIR}/driver/zl38063/firmware")
target_link_libraries(${COMPONENT_TARGET} INTERFACE firmware)
```

That build therefore *requires* the gated `firmware/` static library plus the symbols
`st_twFirmware`, `firmwareStreamLen`, `executionAddress`, `haveProgramBaseAddress`,
`programBaseAddress` (from `zl38063_firmware.h`) and `st_twConfig`, `configStreamLen`
(from `zl38063_config.h`) used by `tw_spi_access.c`.

Note: `espressif/esp-adf@master` no longer contains `components/audio_hal/` at all
(HTTP 404 for `components/audio_hal` and for every zl38063 path at `master`), so
`release/v2.x` is the only branch carrying this driver.
