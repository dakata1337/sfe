project('sfe', 'c', default_options: [
  'debug=true',
  'c_std=c17',
  'warning_level=3',
  # 'b_sanitize=address,undefined',
])

add_project_arguments('-Wno-gnu-statement-expression', language : 'c')

src = ['src/main.c', 'thirdparty/tomlc17/src/tomlc17.c']
inc = include_directories(['include', 'thirdparty/tomlc17/src'])

executable('sfe', sources: src, include_directories: inc)
