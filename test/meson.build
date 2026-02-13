# SPDX-License-Identifier: LGPL-2.1-or-later

# Generate a private loaders.cache so the test can find our module
# without installing it system-wide.
pixbuf_query_loaders = gdk_pixbuf_query_loaders

loaders_cache = custom_target('loaders-cache',
  output: 'loaders.cache',
  command: [pixbuf_query_loaders, pixbufloader_exr],
  capture: true,
  depends: pixbufloader_exr,
)

test_env = environment()
test_env.set('GDK_PIXBUF_MODULE_FILE', loaders_cache.full_path())

test_data_dir = meson.current_source_dir() / 'data'

test_load = executable('test-load', 'test-load.c',
  dependencies: [gdk_pixbuf_dep],
  c_args: ['-DTEST_DATA_DIR="' + test_data_dir + '"'],
)

test('load', test_load,
  env: test_env,
  depends: [loaders_cache],
)
