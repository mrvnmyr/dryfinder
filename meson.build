project('dryfinder', 'cpp',
  default_options : ['cpp_std=c++20', 'warning_level=3']
)

cpp = meson.get_compiler('cpp')
if cpp.get_id() == 'msvc'
  add_project_arguments('/W4', '/permissive-', language: 'cpp')
else
  add_project_arguments('-Wall', '-Wextra', '-Wpedantic', language: 'cpp')
endif

executable('dryfinder',
  ['src/main.cpp'],
  install : false
)
