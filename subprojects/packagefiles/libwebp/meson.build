project('libwebp', 'c',
  version: '1.6.0',
  license: 'BSD-3-Clause',
  default_options: [
    'c_std=c11',
    'warning_level=0',
    'default_library=static',
  ],
)

cc = meson.get_compiler('c')

c_args = ['-DHAVE_CONFIG_H']

if host_machine.system() == 'windows'
  c_args += ['-D_CRT_SECURE_NO_WARNINGS']
endif

webp_inc = include_directories('src', '.')

dsp_sse2 = files(
  'src/dsp/alpha_processing_sse2.c',
  'src/dsp/dec_sse2.c',
  'src/dsp/filters_sse2.c',
  'src/dsp/lossless_sse2.c',
  'src/dsp/rescaler_sse2.c',
  'src/dsp/upsampling_sse2.c',
  'src/dsp/yuv_sse2.c',
  'src/dsp/cost_sse2.c',
  'src/dsp/enc_sse2.c',
  'src/dsp/lossless_enc_sse2.c',
  'src/dsp/ssim_sse2.c',
)

dsp_sse41 = files(
  'src/dsp/alpha_processing_sse41.c',
  'src/dsp/dec_sse41.c',
  'src/dsp/lossless_sse41.c',
  'src/dsp/upsampling_sse41.c',
  'src/dsp/yuv_sse41.c',
  'src/dsp/enc_sse41.c',
  'src/dsp/lossless_enc_sse41.c',
)

dsp_avx2 = files(
  'src/dsp/lossless_avx2.c',
  'src/dsp/lossless_enc_avx2.c',
)

dsp_common = files(
  'src/dsp/alpha_processing.c',
  'src/dsp/cpu.c',
  'src/dsp/dec.c',
  'src/dsp/dec_clip_tables.c',
  'src/dsp/filters.c',
  'src/dsp/lossless.c',
  'src/dsp/rescaler.c',
  'src/dsp/upsampling.c',
  'src/dsp/yuv.c',
)

dsp_enc = files(
  'src/dsp/cost.c',
  'src/dsp/enc.c',
  'src/dsp/lossless_enc.c',
  'src/dsp/ssim.c',
)

dec_sources = files(
  'src/dec/alpha_dec.c',
  'src/dec/buffer_dec.c',
  'src/dec/frame_dec.c',
  'src/dec/idec_dec.c',
  'src/dec/io_dec.c',
  'src/dec/quant_dec.c',
  'src/dec/tree_dec.c',
  'src/dec/vp8_dec.c',
  'src/dec/vp8l_dec.c',
  'src/dec/webp_dec.c',
)

enc_sources = files(
  'src/enc/alpha_enc.c',
  'src/enc/analysis_enc.c',
  'src/enc/backward_references_cost_enc.c',
  'src/enc/backward_references_enc.c',
  'src/enc/config_enc.c',
  'src/enc/cost_enc.c',
  'src/enc/filter_enc.c',
  'src/enc/frame_enc.c',
  'src/enc/histogram_enc.c',
  'src/enc/iterator_enc.c',
  'src/enc/near_lossless_enc.c',
  'src/enc/picture_csp_enc.c',
  'src/enc/picture_enc.c',
  'src/enc/picture_psnr_enc.c',
  'src/enc/picture_rescale_enc.c',
  'src/enc/picture_tools_enc.c',
  'src/enc/predictor_enc.c',
  'src/enc/quant_enc.c',
  'src/enc/syntax_enc.c',
  'src/enc/token_enc.c',
  'src/enc/tree_enc.c',
  'src/enc/vp8l_enc.c',
  'src/enc/webp_enc.c',
)

utils_common = files(
  'src/utils/bit_reader_utils.c',
  'src/utils/color_cache_utils.c',
  'src/utils/filters_utils.c',
  'src/utils/huffman_utils.c',
  'src/utils/palette.c',
  'src/utils/quant_levels_dec_utils.c',
  'src/utils/rescaler_utils.c',
  'src/utils/random_utils.c',
  'src/utils/thread_utils.c',
  'src/utils/utils.c',
)

utils_enc = files(
  'src/utils/bit_writer_utils.c',
  'src/utils/huffman_encode_utils.c',
  'src/utils/quant_levels_utils.c',
)

demux_sources = files(
  'src/demux/anim_decode.c',
  'src/demux/demux.c',
)

sharpyuv_sources = files(
  'sharpyuv/sharpyuv.c',
  'sharpyuv/sharpyuv_csp.c',
  'sharpyuv/sharpyuv_dsp.c',
  'sharpyuv/sharpyuv_gamma.c',
  'sharpyuv/sharpyuv_cpu.c',
  'sharpyuv/sharpyuv_sse2.c',
)

sharpyuv_lib = static_library('sharpyuv',
  sharpyuv_sources,
  include_directories: include_directories('.'),
  c_args: c_args,
  install: false,
)

libwebp_lib = static_library('webp',
  dsp_common, dsp_enc, dsp_sse2, dsp_sse41,
  dec_sources, enc_sources,
  utils_common, utils_enc,
  demux_sources,
  include_directories: webp_inc,
  c_args: c_args,
  link_with: sharpyuv_lib,
  install: false,
)

libwebp_avx2 = static_library('webp_avx2',
  dsp_avx2,
  include_directories: webp_inc,
  c_args: c_args + ['/arch:AVX2'],
  install: false,
)

libwebp = declare_dependency(
  link_with: [libwebp_lib, libwebp_avx2],
  include_directories: webp_inc,
)
