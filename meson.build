project('sfe', 'c',
  version: '0.1.0',
  meson_version: '>= 1.0',
  default_options: [
    'debug=true',
    'c_std=c17',
    'warning_level=3',
    # 'b_sanitize=address,undefined',
  ],
)

gnome = import('gnome')
cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments(
  '-Wno-gnu-statement-expression', # GLib macros (clang)
  '-Wno-missing-field-initializers', # allow partial initializers in tables
  '-Wno-overlength-strings', # generated GResource data
), language: 'c')
add_project_arguments('-DG_LOG_DOMAIN="sfe"', language: 'c')

# Headers of system libraries are included as -isystem so -Wpedantic only
# applies to our own code.
glib_dep = dependency('glib-2.0', version: '>= 2.70', include_type: 'system')
gio_dep  = dependency('gio-unix-2.0', include_type: 'system')
gtk_dep  = dependency('gtk+-3.0', version: '>= 3.24', include_type: 'system')
m_dep    = cc.find_library('m', required: false)

# Vendored TOML parser, built with its own (relaxed) warning level.
tomlc17 = static_library('tomlc17', 'thirdparty/tomlc17/src/tomlc17.c',
  override_options: ['warning_level=0'],
)
tomlc17_dep = declare_dependency(
  link_with: tomlc17,
  include_directories: include_directories('thirdparty/tomlc17/src', is_system: true),
)

conf = configuration_data()
conf.set_quoted('SFE_VERSION', meson.project_version())
conf.set_quoted('SFE_APP_ID', 'org.sfe.SimpleFileExplorer')
configure_file(output: 'sfe-build.h', configuration: conf)

resources = gnome.compile_resources('sfe-resources', 'src/sfe.gresource.xml',
  source_dir: 'src',
  c_name: 'sfe',
)

# Modules that only need GLib (unit-testable without a display).
core_src = files(
  'src/bookmarks.c',
  'src/config.c',
  'src/devices.c',
  'src/history.c',
  'src/opener.c',
  'src/util.c',
)
core_lib = static_library('sfe-core', core_src,
  dependencies: [glib_dep, gio_dep, tomlc17_dep],
)
core_dep = declare_dependency(
  link_with: core_lib,
  include_directories: include_directories('src'),
  dependencies: [glib_dep, gio_dep, tomlc17_dep],
)

ui_src = files(
  'src/main.c',
  'src/app.c',
  'src/dialogs.c',
  'src/dirmodel.c',
  'src/dirtree.c',
  'src/icons.c',
  'src/pane.c',
  'src/pathentry.c',
  'src/sidebar.c',
  'src/split.c',
  'src/theme.c',
  'src/window.c',
)

executable('sfe', ui_src, resources,
  dependencies: [core_dep, gtk_dep, m_dep],
  install: true,
)

# Named after the Wayland app_id (the program name) so compositors and docks
# can match windows to it.
install_data('data/sfe.desktop',
  install_dir: get_option('datadir') / 'applications',
)

subdir('tests')
