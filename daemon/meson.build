# SPDX-License-Identifier: MIT

# ---- the Conexant blobs, prepared for a userspace link ------------------------
#
# blobprep.py renames sections that would break an ordinary link and verifies
# that symbols and relocations are otherwise untouched (docs/BLOB-INTEGRATION.md).

blobprep = files('blob/blobprep.py')
prepared_blobs = {}
foreach b : ['hsfengine', 'hsfhda']
  cmd = [python, blobprep, '@INPUT@', '@OUTPUT@', '--blob', b, '--objcopy', objcopy]
  if get_option('verify_blob_checksums')
    cmd += ['--manifest', linuxant / 'SHA256SUMS']
  endif
  prepared_blobs += {b: custom_target(b + '.o',
    input: blob_dir / '@0@-@1@.O'.format(b, arch),
    output: b + '.o',
    command: cmd,
  )}
endforeach

ctors_ld = meson.current_source_dir() / 'blob' / 'hsf-ctors.ld'
# Non-PIE: the blobs use 32-bit absolute relocations (R_X86_64_32S/R_386_32).
blob_link_args = ['-no-pie', '-Wl,-z,noexecstack', '-Wl,-T,' + ctors_ld]
blob_link_depends = files('blob/hsf-ctors.ld')

# ---- the OS layer -------------------------------------------------------------

cfgnames = custom_target('cfgnames.inc',
  input: 'blob/gen_cfgnames.py',
  output: 'cfgnames.inc',
  command: [python, '@INPUT@', '@OUTPUT@', '--'] + cc.cmd_array() + arch_args + blob_abi_args
           + ['-isystem', linuxant / 'include'],
)

os_inc = include_directories('os')

libhsfos = static_library('hsfos',
  'os/os_atomic.c',
  'os/os_fault.c',
  'os/os_float.c',
  'os/os_init.c',
  'os/os_log.c',
  'os/os_mem.c',
  'os/os_pci.c',
  'os/os_physmem.c',
  'os/os_str.c',
  'os/os_stubs.c',
  'os/os_sync.c',
  'os/os_thread.c',
  'os/os_time.c',
  'os/os_timer.c',
  'nvm/nvm.c',
  cfgnames,
  include_directories: os_inc,
  dependencies: [blob_abi_dep, threads],
  c_args: ['-DHSF_NVM_STATIC_DIR="@0@"'.format(nvm_static_dir),
           '-DHSF_NVM_DYNAMIC_DIR="@0@"'.format(nvm_dynamic_dir)],
  pic: false,
)

hsfos_dep = declare_dependency(
  link_with: libhsfos,
  include_directories: os_inc,
  dependencies: [blob_abi_dep, threads],
)

# ---- HD-audio backends behind the OsHdaCodec* pointers ------------------------

hda_inc = include_directories('hda')
uapi_inc = include_directories('../include/uapi')

libhsfhda = static_library('hsfhda',
  'hda/hda_alsa.c',
  'hda/hda_fake.c',
  'hda/hda_glue.c',
  'hda/hda_record.c',
  include_directories: [hda_inc, uapi_inc],
  dependencies: hsfos_dep,
  pic: false,
)

hsfhda_dep = declare_dependency(
  link_with: libhsfhda,
  include_directories: hda_inc,
  dependencies: hsfos_dep,
)

# ---- the serial bridge and the simulated modem ---------------------------------

serial_inc = include_directories('serial')

libhsfserial = static_library('hsfserial',
  'serial/bridge.c',
  'serial/port.c',
  'serial/sim_port.c',
  include_directories: serial_inc,
  dependencies: hsfos_dep,
  pic: false,
)

hsfserial_dep = declare_dependency(
  link_with: libhsfserial,
  include_directories: serial_inc,
  dependencies: hsfos_dep,
)

# ---- hsfmodemd ------------------------------------------------------------------

hsfmodemd = executable('hsfmodemd',
  'config.c',
  'control.c',
  'engine/modem.c',
  'main.c',
  'privs.c',
  objects: [prepared_blobs['hsfengine'], prepared_blobs['hsfhda']],
  include_directories: [include_directories('engine'), uapi_inc],
  dependencies: [hsfhda_dep, hsfserial_dep],
  c_args: ['-DHSF_VERSION="@0@"'.format(meson.project_version()),
           '-DHSF_NVM_STATIC_DIR="@0@"'.format(nvm_static_dir),
           '-DHSF_NVM_DYNAMIC_DIR="@0@"'.format(nvm_dynamic_dir)],
  link_args: blob_link_args,
  link_depends: blob_link_depends,
  install: true,
  install_dir: get_option('sbindir'),
)
