# runtime/gfx/nvn/host

The host graphics layer: its interface and the null back end, present and window, the programs' prepared SPIR-V and
the texts the host shows.

| File | What it is |
|---|---|
| `gfx.h`, `gfx_null.c` | Graphics layer interface, null back end (no GPU) and `gfx_backend`, which picks the back end and wraps it with the capture of `../cap/` (`MKW_CAPTURE`) |
| `gfx_present.c`, `gfx_wsys.h`, `gfx_window_sdl.c` | Swapchain, present and vsync; SDL3 window |
| `gfx_hdr.c` | HDR output (`MKW_HDR` auto/off): what nn::vi tells the game about the display's HDR (`MKW_HDR_FAKE`), the HDR10 swapchain format and metadata, and 10-bit PQ pixels to sRGB bytes for shots, captures and thumbnails |
| `gfx_shader.c` | Loads MKW's prepared SPIR-V (`prepared_get`, key `nvn2_program_key`), variants with integer inputs, integer outputs or integer images and reflection (`.res.json` and the SPIR-V itself); failed integer-image conversions name the program and reason and save their input with `MKW_SPIRV_INT_DUMP`; loads the pipeline prewarm shaders in parallel (`gfx_shader_prewarm`) |
| `gfx_spirv_io.c` | The integer-inputs patch of a vertex shader and the integer-outputs patch of a fragment shader: the inputs whose attribute format, or the outputs whose colour target, is UINT/SINT become u/ivec4, their loads and stores bitcast (issue #64) |
| `gfx_spirv_img.c` | The integer-images patch of a compute program: the textures and storage images whose bound view is UINT/SINT become integer images, including separate image/sampler construction, their reads and writes bitcast (VUID format-07753, issue #64); failures return a diagnostic reason |
| `ui_text.c`, `ui_text.h` | Texts the host shows the player (software keyboard, menu and start screen; menu in English, Russian and Spanish) |
| `ui_text_cat.h` | What every language's catalog shares (the language names, the arrays' declarations); the catalogs themselves are generated from `i18n/*.po` |
