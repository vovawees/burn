project('btmeson', 'c', version: '1.0')
executable('btmeson', 'main.c', c_args: ['-DBT_FOO=' + (get_option('foo') ? '1' : '0')], install: true)
install_data('btmeson.conf', install_dir: get_option('sysconfdir'))
