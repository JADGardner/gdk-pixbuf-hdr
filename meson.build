# SPDX-License-Identifier: LGPL-2.1-or-later
project('gdk-pixbuf-exr', 'c',
  version: '0.1.0',
  license: 'LGPL-2.1-or-later',
  meson_version: '>= 0.60',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')

# Dependencies
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', version: '>= 2.36')

# tinyexr may not have pkg-config
tinyexr_dep = dependency('tinyexr', required: false)
if not tinyexr_dep.found()
  tinyexr_dep = cc.find_library('tinyexr')
endif

# Hardening flags
extra_c_args = cc.get_supported_arguments([
  '-Wconversion',
  '-Wsign-conversion',
  '-Wformat=2',
  '-Wformat-security',
  '-fstack-protector-strong',
])

# _FORTIFY_SOURCE requires optimization
if get_option('optimization') != '0'
  extra_c_args += cc.get_supported_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3')
endif

add_project_arguments(extra_c_args, language: 'c')

# Loader install path
gdk_pixbuf_binary_version = gdk_pixbuf_dep.get_variable(pkgconfig: 'gdk_pixbuf_binary_version')
loader_dir = get_option('libdir') / 'gdk-pixbuf-2.0' / gdk_pixbuf_binary_version / 'loaders'

# Build the loader module
pixbufloader_exr = shared_module('pixbufloader-exr',
  'io-exr.c',
  dependencies: [gdk_pixbuf_dep, tinyexr_dep, cc.find_library('m', required: false)],
  install: true,
  install_dir: loader_dir,
  name_prefix: '',
  gnu_symbol_visibility: 'hidden',
)

# Thumbnailer
install_data('exr.thumbnailer',
  install_dir: get_option('datadir') / 'thumbnailers',
)

# Update loaders cache on install
gdk_pixbuf_query_loaders = find_program(
  'gdk-pixbuf-query-loaders',
  gdk_pixbuf_dep.get_variable(pkgconfig: 'gdk_pixbuf_query_loaders'),
)
meson.add_install_script(gdk_pixbuf_query_loaders, '--update-cache',
  skip_if_destdir: true,
)

# Tests
if get_option('tests')
  subdir('test')
endif
