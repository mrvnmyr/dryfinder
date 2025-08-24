project('dryfinder', 'cpp',
  default_options : ['cpp_std=c++20', 'warning_level=3']
)

cpp = meson.get_compiler('cpp')
message('Configuring for compiler: ' + cpp.get_id())

if cpp.get_id() == 'msvc'
  # Rely on Meson's warning_level=3 (maps to /W3). Keep permissive- for
  # stricter standard conformance.
  add_project_arguments('/permissive-', language: 'cpp')
  message('MSVC: using warning_level=3 (no explicit /W4); added /permissive-')
else
  # For GCC/Clang, warning_level=3 already maps to -Wall -Wextra. Keep only
  # -Wpedantic explicitly for additional pedantic checks.
  add_project_arguments('-Wpedantic', language: 'cpp')
  message('Non-MSVC: using warning_level=3 (+ -Wpedantic)')
endif

executable('dryfinder',
  ['main.cpp'],
  install : false
)
