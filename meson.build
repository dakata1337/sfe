project(
    'sfe',
    'c',
    version: '3.2.4',
    meson_version: '>= 1.0',
    default_options: [
        # Optimized (-O3), no debug info, NDEBUG; for development:
        #   meson setup build -Dbuildtype=debug
        'buildtype=release',
        'b_ndebug=if-release',
        'c_std=c17',
        'warning_level=3',
        # 'b_sanitize=address,undefined',
    ],
)

gnome = import('gnome')
cc = meson.get_compiler('c')

# Debug builds (debug / debugoptimized) log sfe's messages (main.c). The
# packages build with buildtype=plain (arch-meson), which must not.
if get_option('debug')
    add_project_arguments('-DDEBUG', language: 'c')
    add_project_arguments('-DBUILD_TYPE="debug"', language: 'c')
else
    add_project_arguments('-DBUILD_TYPE="release"', language: 'c')
endif

add_project_arguments(
    cc.get_supported_arguments(
        '-Wno-gnu-statement-expression', # GLib macros (clang)
        '-Wno-missing-field-initializers', # allow partial initializers in tables
        '-Wno-overlength-strings', # generated GResource data
    ),
    language: 'c',
)
add_project_arguments('-DG_LOG_DOMAIN="sfe"', language: 'c')

# Fuzzing: the code under test gets coverage instrumentation; the targets
# in tests/fuzz/ link libFuzzer. Sanitizers come from b_sanitize (tools/fuzz.sh).
if get_option('fuzzing')
    if cc.get_id() != 'clang'
        error('-Dfuzzing=true needs clang: CC=clang meson setup ...')
    endif
    add_project_arguments('-fsanitize=fuzzer-no-link', language: 'c')
endif

# Headers of system libraries are included as -isystem so -Wpedantic only
# applies to our own code.
glib_dep = dependency('glib-2.0', version: '>= 2.70', include_type: 'system')
gio_dep = dependency('gio-unix-2.0', include_type: 'system')
gtk_dep = dependency('gtk+-3.0', version: '>= 3.24', include_type: 'system')
pixbuf_dep = dependency('gdk-pixbuf-2.0', include_type: 'system')
m_dep = cc.find_library('m', required: false)

# Vendored TOML parser, built with its own (relaxed) warning level.
tomlc17 = static_library(
    'tomlc17',
    'thirdparty/tomlc17/src/tomlc17.c',
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

# The git commit, shown in Settings → System. vcs_tag() asks git on every
# build (so it stays current); without git or a .git folder it falls back to
# data/git-commit, which `git archive` (GitHub's release tarballs) fills in
# through export-subst (.gitattributes), else "unknown".
archived_commit = import('fs').read('data/git-commit').strip()
commit_h = vcs_tag(
    command: ['git', '-C', meson.project_source_root(), 'rev-parse', '--short=12', 'HEAD'],
    input: 'src/sfe-commit.h.in',
    output: 'sfe-commit.h',
    replace_string: '@SFE_GIT_COMMIT@',
    fallback: archived_commit.startswith('$') ? 'unknown' : archived_commit.substring(0, 12),
)

resources = gnome.compile_resources(
    'sfe-resources',
    'src/sfe.gresource.xml',
    source_dir: 'src',
    c_name: 'sfe',
)

# Modules that only need GLib (unit-testable without a display).
core_src = files(
    'src/archive.c',
    'src/bookmarks.c',
    'src/chooser.c',
    'src/cmdline.c',
    'src/config.c',
    'src/devices.c',
    'src/filefilter.c',
    'src/fileops.c',
    'src/fuzzy.c',
    'src/history.c',
    'src/keymap.c',
    'src/opener.c',
    'src/portalconf.c',
    'src/portalreq.c',
    'src/rename.c',
    'src/sizecount.c',
    'src/thumbs.c',
    'src/trash.c',
    'src/undo.c',
    'src/util.c',
)
core_lib = static_library(
    'sfe-core',
    core_src,
    dependencies: [glib_dep, gio_dep, pixbuf_dep, tomlc17_dep],
)
core_dep = declare_dependency(
    link_with: core_lib,
    include_directories: include_directories('src'),
    dependencies: [glib_dep, gio_dep, pixbuf_dep, tomlc17_dep],
)

ui_src = files(
    'src/actions.c',
    'src/app.c',
    'src/clipboard.c',
    'src/cmdbar.c',
    'src/commands.c',
    'src/compressdlg.c',
    'src/ctxmenu.c',
    'src/defaultfm.c',
    'src/dialogs.c',
    'src/dirmodel.c',
    'src/dirtree.c',
    'src/icons.c',
    'src/opsbar.c',
    'src/pane.c',
    'src/pathentry.c',
    'src/prefs.c',
    'src/preview.c',
    'src/props.c',
    'src/renamedlg.c',
    'src/search.c',
    'src/sidebar.c',
    'src/split.c',
    'src/theme.c',
    'src/window.c',
)

executable(
    'sfe',
    ui_src,
    'src/main.c', # (not in ui_src: tests/test_ui.c links ui_src with its own main)
    resources,
    commit_h,
    dependencies: [core_dep, gtk_dep, m_dep],
    install: true,
)

# Named after the Wayland app_id (the program name) so compositors and docks
# can match windows to it.
install_data(
    'data/sfe.desktop',
    install_dir: get_option('datadir') / 'applications',
)

# The file dialog backend for xdg-desktop-portal (src/portal/sfe-portal.c).
# Installed, it is only used when portals.conf names it for FileChooser.
executable(
    'sfe-portal',
    'src/portal/sfe-portal.c',
    'src/portalreq.c',
    include_directories: include_directories('src'),
    dependencies: [glib_dep, gio_dep],
    c_args: [
        '-DSFE_BINDIR="@0@"'.format(get_option('prefix') / get_option('bindir')),
    ],
    install: true,
    install_dir: get_option('libexecdir'),
)
install_data(
    'data/portal/sfe.portal',
    install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals',
)
portal_service = configuration_data()
portal_service.set('libexecdir', get_option('prefix') / get_option('libexecdir'))
configure_file(
    input: 'data/portal/org.freedesktop.impl.portal.desktop.sfe.service.in',
    output: 'org.freedesktop.impl.portal.desktop.sfe.service',
    configuration: portal_service,
    install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

# Software centers / package tools.
install_data(
    'data/org.sfe.SimpleFileExplorer.metainfo.xml',
    install_dir: get_option('datadir') / 'metainfo',
)

install_man('docs/sfe.1')

# The user guide, readable offline (its screenshot tools stay out).
install_subdir(
    'docs/guide',
    install_dir: get_option('datadir') / 'doc' / meson.project_name(),
    exclude_directories: ['tools'],
)

# The logo: icons/hicolor/{scalable,symbolic}/apps/sfe[-symbolic].svg
install_subdir(
    'data/icons/hicolor',
    install_dir: get_option('datadir') / 'icons',
)

# Refreshes the icon cache and desktop database on a plain `meson install`
# (skipped when packaging with DESTDIR; pacman hooks do it then).
gnome.post_install(gtk_update_icon_cache: true, update_desktop_database: true)

subdir('tests')
