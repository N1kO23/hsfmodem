# SPDX-License-Identifier: MIT

sbindir = get_option('prefix') / get_option('sbindir')
sysconfdir = get_option('sysconfdir')

install_data('hsfmodemd.conf', install_dir: sysconfdir / 'hsfmodem')

if get_option('openrc')
  configure_file(input: 'openrc/hsfmodemd.initd.in', output: 'hsfmodemd',
    configuration: {'SBINDIR': sbindir},
    install: true, install_dir: sysconfdir / 'init.d', install_mode: 'rwxr-xr-x',
    install_tag: 'runtime')
  install_data('openrc/hsfmodemd.confd', rename: 'hsfmodemd', install_dir: sysconfdir / 'conf.d')
endif

if get_option('systemd')
  systemd = dependency('systemd', required: false)
  unitdir = systemd.found() ? systemd.get_variable('systemdsystemunitdir') \
                            : get_option('prefix') / 'lib' / 'systemd' / 'system'
  sysusersdir = systemd.found() ? systemd.get_variable('sysusersdir') \
                                : get_option('prefix') / 'lib' / 'sysusers.d'
  configure_file(input: 'systemd/hsfmodemd.service.in', output: 'hsfmodemd.service',
    configuration: {'SBINDIR': sbindir},
    install: true, install_dir: unitdir)
  install_data('systemd/hsfmodem.sysusers', rename: 'hsfmodem.conf', install_dir: sysusersdir)
endif
