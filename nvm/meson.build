# SPDX-License-Identifier: MIT
# The static NVM parameter tree for the hsfhda profile, generated from the
# Conexant .inf/.cty files by the unchanged Linuxant cvtinf.pl.

perl = find_program('perl')
inf_dir = linuxant / 'inf'
cnxt_version = arch == 'x86_64' ? '7.80.02.05x86_64full' : '7.80.02.05full'
nvm_tree_dir = meson.current_build_dir() / 'nvm-static'

nvm_tree = custom_target('nvm-static',
  input: files('build_tree.py', 'cvtinf.pl',
               inf_dir / 'hsfpcibasic2.inf', inf_dir / 'hsfpcibasic2smart.inf',
               inf_dir / 'hsfhda.inf', inf_dir / 'hsf.cty'),
  output: 'nvm-static.stamp',
  command: [python, '@INPUT0@', '--cvtinf', '@INPUT1@', '--inf-dir', inf_dir,
            '--out', nvm_tree_dir, '--version', cnxt_version, '--stamp', '@OUTPUT@'],
  build_by_default: true,
)

meson.add_install_script(python, files('install_tree.py'), nvm_tree_dir, nvm_static_dir,
                         skip_if_destdir: false)
