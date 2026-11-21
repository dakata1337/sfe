# One libFuzzer target per parser; see README.md. Only with -Dfuzzing=true.
foreach name : ['config', 'cmdline', 'fuzzy', 'keymap', 'filefilter', 'rename', 'portalreq']
    executable('fuzz-' + name, 'fuzz_' + name + '.c',
        dependencies: core_dep,
        link_args: '-fsanitize=fuzzer',
        build_by_default: true,
    )
endforeach
