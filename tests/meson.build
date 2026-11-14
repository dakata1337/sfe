test_config = executable('test-config', 'test_config.c',
  dependencies: core_dep,
)
test('config', test_config)

test_nav = executable('test-nav', 'test_nav.c',
  dependencies: core_dep,
)
test('nav', test_nav)

test_bookmarks = executable('test-bookmarks', 'test_bookmarks.c',
  dependencies: core_dep,
)
test('bookmarks', test_bookmarks)

test_fileops = executable('test-fileops', 'test_fileops.c',
  dependencies: core_dep,
)
test('fileops', test_fileops)

test_rename = executable('test-rename', 'test_rename.c',
  dependencies: core_dep,
)
test('rename', test_rename)

test_fuzzy = executable('test-fuzzy', 'test_fuzzy.c',
  dependencies: core_dep,
)
test('fuzzy', test_fuzzy)

# The user guide must list every default shortcut.
python = find_program('python3')
test('guide', python, args: [files('check_guide.py'), meson.project_source_root()])

test_archive = executable('test-archive', 'test_archive.c',
  dependencies: core_dep,
)
test('archive', test_archive)

test_trash = executable('test-trash', 'test_trash.c',
  dependencies: core_dep,
)
test('trash', test_trash)
