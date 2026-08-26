app_id = 'io.github.Aihuman.ExtMon'

install_data(app_id + '.desktop',
  install_dir: get_option('datadir') / 'applications')

icon_sizes = ['16x16', '32x32', '48x48', '64x64', '128x128', '256x256', '512x512']
foreach size : icon_sizes
  install_data('icons/hicolor' / size / 'apps' / app_id + '.png',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / size / 'apps')
endforeach

install_data('icons/extmon.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
  rename: app_id + '.svg')
