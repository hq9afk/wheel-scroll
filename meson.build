project('wheel-scroll', 'cpp',
  version : '0.3.0',
  license : 'MIT',
  default_options : [
    'cpp_std=c++17',
    'warning_level=3',
    'buildtype=release',
  ],
)

libevdev_dep = dependency('libevdev')

executable('wheel-scroll', 'main.cpp',
  dependencies : [libevdev_dep],
  install : true,
  install_dir : get_option('bindir'),
)
