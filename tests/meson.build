# Test programs are built by `meson test` only, not by a plain build
# (the packages' build step).
test_config = executable('test-config', 'test_config.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('config', test_config)

test_nav = executable('test-nav', 'test_nav.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('nav', test_nav)

test_bookmarks = executable('test-bookmarks', 'test_bookmarks.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('bookmarks', test_bookmarks)

test_fileops = executable('test-fileops', 'test_fileops.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('fileops', test_fileops)

test_rename = executable('test-rename', 'test_rename.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('rename', test_rename)

test_fuzzy = executable('test-fuzzy', 'test_fuzzy.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('fuzzy', test_fuzzy)

# The user guide must list every default shortcut.
python = find_program('python3')
test('guide', python, args: [files('check_guide.py'), meson.project_source_root()])

test_archive = executable('test-archive', 'test_archive.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('archive', test_archive)

test_trash = executable('test-trash', 'test_trash.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('trash', test_trash)

test_keymap = executable('test-keymap', 'test_keymap.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('keymap', test_keymap)

test_cmdline = executable('test-cmdline', 'test_cmdline.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('cmdline', test_cmdline)

test_undo = executable('test-undo', 'test_undo.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('undo', test_undo)

test_thumbs = executable('test-thumbs', 'test_thumbs.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('thumbs', test_thumbs)

# The data files are valid (when the validators are installed).
desktop_validate = find_program('desktop-file-validate', required: false)
if desktop_validate.found()
  test('desktop-file', desktop_validate, args: [meson.project_source_root() / 'data' / 'sfe.desktop'])
endif
appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('metainfo', appstreamcli, args: ['validate', '--no-net', meson.project_source_root() / 'data' / 'org.sfe.SimpleFileExplorer.metainfo.xml'])
endif

test_portalconf = executable('test-portalconf', 'test_portalconf.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('portalconf', test_portalconf)

test_filefilter = executable('test-filefilter', 'test_filefilter.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('filefilter', test_filefilter)

test_portalreq = executable('test-portalreq', 'test_portalreq.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('portalreq', test_portalreq)

test_sizecount = executable('test-sizecount', 'test_sizecount.c',
  dependencies: core_dep,
  build_by_default: false,
)
test('sizecount', test_sizecount)

# Needs a display (icons); skipped (exit 77) without one.
test_dirmodel = executable('test-dirmodel',
  'test_dirmodel.c', '../src/dirmodel.c', '../src/icons.c',
  dependencies: [core_dep, gtk_dep, m_dep],
  build_by_default: false,
)
test('dirmodel', test_dirmodel)
