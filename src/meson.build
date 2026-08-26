executable('extmon',
  'extmon.cpp',
  dependencies: [gtk_dep, gio_dep],
  install: true)

install_data('extmon.py',
  install_dir: get_option('bindir'),
  rename: 'extmon-cli')
