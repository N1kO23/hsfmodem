# SPDX-License-Identifier: MIT

hsfmodem_config = configure_file(
  input: 'hsfmodem-config',
  output: 'hsfmodem-config',
  configuration: {'NVM_STATIC_DIR': nvm_static_dir, 'NVM_DYNAMIC_DIR': nvm_dynamic_dir},
  install: true,
  install_dir: get_option('sbindir'),
  install_mode: 'rwxr-xr-x',
)
hsfmodem_config_path = meson.current_build_dir() / 'hsfmodem-config'
