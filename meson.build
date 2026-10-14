project('nodefs', 'c')

nodefs_cc = meson.get_compiler('c')
nodefs_fs = import('fs')
nodefs_threads_dep = dependency('threads')

nodefs_fuse3_root = get_option('fuse3_root')
if meson.is_cross_build()
  if nodefs_fuse3_root == ''
    error('Cross builds require -Dfuse3_root=<target FUSE3 root>')
  endif

  if not nodefs_fs.is_absolute(nodefs_fuse3_root)
    nodefs_fuse3_root = join_paths(
      meson.project_source_root(),
      nodefs_fuse3_root,
    )
  endif

  nodefs_fuse3_lib = nodefs_cc.find_library(
    'fuse3',
    dirs: [nodefs_fuse3_root],
    required: true,
  )
  nodefs_fuse3_dep = declare_dependency(
    compile_args: ['-I' + nodefs_fuse3_root],
    dependencies: [nodefs_fuse3_lib, nodefs_threads_dep],
  )
else
  if nodefs_fuse3_root != ''
    error('-Dfuse3_root is only supported for cross builds; native builds use pkg-config')
  endif

  nodefs_fuse3_pkg_dep = dependency(
    'fuse3',
    method: 'pkg-config',
    required: true,
    not_found_message: 'FUSE3 was not found. Install the fuse3 development package.',
  )
  nodefs_fuse3_dep = declare_dependency(
    dependencies: [nodefs_fuse3_pkg_dep, nodefs_threads_dep],
  )
endif

nodefs_inc = include_directories('include')

nodefs_lib = static_library(
  'nodefs',
  'src/nodefs.c',
  dependencies: nodefs_fuse3_dep,
  include_directories: nodefs_inc,
  install: true,
)

nodefs_dep = declare_dependency(
  dependencies: nodefs_fuse3_dep,
  include_directories: nodefs_inc,
  link_with: nodefs_lib,
)

nodefs_smoke_test = executable(
  'nodefs_smoke_test',
  'test/smoke_test.c',
  dependencies: nodefs_dep,
)

test('nodefs-smoke', nodefs_smoke_test)

nodefs_poll_test = executable(
  'nodefs_poll_test',
  'test/poll_test.c',
  dependencies: nodefs_fuse3_dep,
  include_directories: nodefs_inc,
)
test('nodefs-poll', nodefs_poll_test)

install_headers('include/nodefs.h')

nodefs_python = import('python').find_installation()
custom_target(
  'nodefs-archive',
  input: [nodefs_lib, nodefs_smoke_test, 'include/nodefs.h'],
  output: 'nodefs-' + host_machine.cpu_family() + '.tar.gz',
  command: [
    nodefs_python,
    files('meson/package.py'),
    '@OUTPUT@',
    '@INPUT@',
  ],
  build_by_default: true,
)
