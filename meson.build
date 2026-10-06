# SPDX-License-Identifier: MIT
project('hsfmodem', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.1',
  default_options: [
    'c_std=gnu17',
    'warning_level=2',
    'werror=true',
    'buildtype=debugoptimized',
    'b_pie=false',
  ],
)

cc = meson.get_compiler('c')
fs = import('fs')
python = find_program('python3')
objcopy = find_program('objcopy')

arch = get_option('arch')
if arch == 'auto'
  family = host_machine.cpu_family()
  if family == 'x86_64'
    arch = 'x86_64'
  elif family == 'x86'
    arch = 'i386'
  else
    error('The Conexant blobs exist only for x86_64 and i386, not @0@'.format(family))
  endif
endif

# Symbol prefix version token used by the blobs (see osuniqredef.h).
blob_version = arch == 'x86_64' ? '7800205x86_64full' : '7800205full'

linuxant = meson.project_source_root() / 'third_party' / 'linuxant'
blob_dir = get_option('blob_dir') != '' ? get_option('blob_dir') : linuxant / 'blobs'

datadir = get_option('prefix') / get_option('datadir')
nvm_static_dir = get_option('nvm_static_dir') != '' ? get_option('nvm_static_dir') : datadir / 'hsfmodem' / 'nvm'
nvm_dynamic_dir = get_option('nvm_dynamic_dir')

# The blobs are built for a 4-byte aligned i386 stack and plain cdecl calls.
arch_args = arch == 'i386' ? ['-march=i686', '-mstackrealign'] : []
add_project_arguments(arch_args + ['-fno-strict-aliasing', '-D_GNU_SOURCE'], language: 'c')

# Defines the blobs were compiled with (third_party/linuxant/makeflags-*.mak),
# applied whenever blob ABI headers are included.
blob_abi_args = [
  '-DFRAME_WORK=FWK_LINUX_SOFTK56', '-DTARGET_HSF_LINUX', '-DUSE_HSF_MODULATIONS',
  '-DMT_SUPPORT', '-DUSE_ORCA_V32', '-DUSE_ORCA_FSK', '-DUSE_ORCA_V17',
  '-DNO_DUMPMGR_SUPPORT', '-DNO_BLAM_SUPPORT', '-DFRAME_WORK_IMPORTED', '-DNOASSERTS',
  '-DUSE_TRACE_ONLY=T__FATAL_ERROR|T__ERROR', '-DUSE_TRACE',
  '-DOS_NOREDEFS', '-DOS_NOSTRINGREDEFS',
  '-DCNXTLINUXVERSION_NODOT=' + blob_version, '-DCNXTTARGET="hsf"',
  '-imacros', linuxant / 'include' / 'framewrk.h',
]
blob_abi_dep = declare_dependency(
  compile_args: blob_abi_args,
  include_directories: include_directories('third_party/linuxant/include', is_system: true),
)

threads = dependency('threads')

subdir('daemon')
subdir('nvm')
subdir('tools')
subdir('data')
if get_option('tests')
  subdir('tests')
endif

summary({
  'blob architecture': arch,
  'blob directory': blob_dir,
  'static NVM': nvm_static_dir,
  'dynamic NVM': nvm_dynamic_dir,
}, section: 'hsfmodem')
