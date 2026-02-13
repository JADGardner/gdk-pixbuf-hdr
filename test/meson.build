# SPDX-License-Identifier: LGPL-2.1-or-later

# Generate a private loaders.cache so the test can find our modules
# without installing them system-wide.
pixbuf_query_loaders = gdk_pixbuf_query_loaders

loaders_cache = custom_target('loaders-cache',
  output: 'loaders.cache',
  command: [pixbuf_query_loaders, pixbufloader_exr, pixbufloader_hdr],
  capture: true,
  depends: [pixbufloader_exr, pixbufloader_hdr],
)

# Build a local MIME database so GIO can resolve image/vnd.radiance.
# gdk-pixbuf uses g_content_type_guess() (GIO) for format detection,
# which consults the shared-mime-info database.  Without this, the HDR
# loader's MIME type won't be recognised in the test environment.
update_mime_database = find_program('update-mime-database', required: false)
if update_mime_database.found()
  mime_dir = meson.current_build_dir() / 'mime-data' / 'mime'
  mime_db = custom_target('mime-database',
    output: 'mime-data',
    command: [
      'sh', '-c',
      'mkdir -p "$1/packages" && cp "$2" "$1/packages/" && "$3" "$1"',
      '--',
      mime_dir,
      meson.project_source_root() / 'gdk-pixbuf-hdr.xml',
      update_mime_database.full_path(),
    ],
  )
else
  mime_db = []
endif

test_env = environment()
test_env.set('GDK_PIXBUF_MODULE_FILE', loaders_cache.full_path())
# Point GIO at our local MIME database (prepend to existing data dirs)
test_env.set('XDG_DATA_HOME', meson.current_build_dir() / 'mime-data')

test_data_dir = meson.current_source_dir() / 'data'

test_load = executable('test-load', 'test-load.c',
  dependencies: [gdk_pixbuf_dep],
  c_args: ['-DTEST_DATA_DIR="' + test_data_dir + '"'],
)

test('load', test_load,
  env: test_env,
  depends: [loaders_cache, mime_db],
)
