# SPDX-License-Identifier: MIT

# ---- unit tests: the OS layer on its own -------------------------------------

unit_tests = {
  'os_basic': 'unit/test_os_basic.c',
  'os_sync': 'unit/test_os_sync.c',
  'os_thread': 'unit/test_os_thread.c',
}
foreach name, src : unit_tests
  test(name,
    executable('test_' + name, src, dependencies: hsfos_dep),
    protocol: 'tap',
    suite: 'unit',
    timeout: 60,
  )
endforeach

test('nvm',
  executable('test_nvm', 'unit/test_nvm.c', dependencies: hsfos_dep),
  protocol: 'tap',
  suite: 'unit',
  depends: nvm_tree,
  env: {'HSF_TEST_NVM_STATIC': nvm_tree_dir},
)

test('hda',
  executable('test_hda', 'unit/test_hda.c', dependencies: hsfhda_dep),
  protocol: 'tap',
  suite: 'unit',
)

test('bridge',
  executable('test_bridge', 'unit/test_bridge.c', dependencies: hsfserial_dep),
  protocol: 'tap',
  suite: 'unit',
  timeout: 60,
)

test('uapi',
  executable('test_uapi', 'unit/test_uapi.c',
             include_directories: include_directories('../include/uapi')),
  protocol: 'tap',
  suite: 'unit',
)

# The crash reporter resolves addresses with the executable's own symbol
# table, which requires a non-PIE executable like the daemon.
test('fault',
  executable('test_fault', 'unit/test_fault.c', dependencies: hsfos_dep, link_args: ['-no-pie']),
  protocol: 'tap',
  suite: 'unit',
)

# ---- contract tests: the real blobs against the OS layer and HDA backends -----

test_engine = executable('test_engine',
  'contract/test_engine.c',
  objects: [prepared_blobs['hsfengine'], prepared_blobs['hsfhda']],
  dependencies: hsfhda_dep,
  link_args: blob_link_args,
  link_depends: blob_link_depends,
)

test('engine', test_engine,
  protocol: 'tap',
  suite: 'contract',
  depends: nvm_tree,
  env: {'HSF_TEST_NVM_STATIC': nvm_tree_dir},
  timeout: 120,
)

test('blob_link_is_exec', python,
  args: [files('check_exec.py'), test_engine],
  suite: 'contract',
)

# ---- integration: the daemon as a process ------------------------------------

test('daemon', python,
  args: [files('integration/test_daemon.py')],
  env: {'HSFMODEMD': hsfmodemd.full_path(), 'HSF_TEST_NVM_STATIC': nvm_tree_dir},
  depends: [hsfmodemd, nvm_tree],
  suite: 'integration',
  timeout: 180,
)

test('config_tool', python,
  args: [files('integration/test_config_tool.py')],
  env: {'HSFMODEM_CONFIG': hsfmodem_config_path, 'HSFMODEMD': hsfmodemd.full_path(),
        'HSF_TEST_NVM_STATIC': nvm_tree_dir},
  depends: [hsfmodemd, nvm_tree],
  suite: 'integration',
  timeout: 120,
)
