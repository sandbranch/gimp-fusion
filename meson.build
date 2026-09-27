project('gimp-fusion', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.60.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'],
)

cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: false)
# OpenMP for the loops over pixels; without it everything runs on one core
omp_dep = dependency('openmp', required: get_option('openmp'))

# the core: exposure fusion, focus stacking, alignment; no GIMP, no GEGL
core_lib = static_library('fusion-core',
  'src/core/image.c',
  'src/core/pyramid.c',
  'src/core/fusion.c',
  'src/core/align.c',
  'src/core/fft.c',
  dependencies: [math_dep, omp_dep],
  c_args: cc.get_supported_arguments('-ffp-contract=off'),
)
core_dep = declare_dependency(link_with: core_lib,
  include_directories: include_directories('src/core'),
  dependencies: [math_dep, omp_dep])

# the core on the command line (PFM files), for the tests and benchmarks
fusion_cli = executable('fusion-cli', 'tools/fusion-cli.c',
  dependencies: core_dep,
  install: false,
)

# unit tests of the core: meson test, or tests/unit.sh (also under ASan)
foreach t : ['pyramid', 'fusion', 'align']
  test(t, executable('test-' + t, 'tests/unit/test-' + t + '.c',
                     'tests/unit/common.c',
                     dependencies: core_dep),
       timeout: 600)
endforeach

gimp_dep = dependency('gimp-3.0', version: '>= 3.0.0', required: get_option('plugin'))
gimpui_dep = dependency('gimpui-3.0', version: '>= 3.0.0', required: get_option('plugin'))

if gimp_dep.found() and gimpui_dep.found()
  plugindir = get_option('plugindir')
  if plugindir == ''
    plugindir = gimp_dep.get_variable(pkgconfig: 'gimplibdir') / 'plug-ins'
  endif
  # GIMP 3 loads a plug-in from a folder of its own name
  executable('gimp-fusion',
    'src/plugin/main.c',
    'src/plugin/layers.c',
    dependencies: [core_dep, gimp_dep, gimpui_dep],
    c_args: ['-Wno-unused-parameter'],
    install: true,
    install_dir: plugindir / 'gimp-fusion',
  )
endif
