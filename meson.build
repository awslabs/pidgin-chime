project('chime', 'c',
  version: '3.28.1',
  meson_version: '>= 0.43.0',
  license: 'GPL2+'
)

chime_version = meson.project_version()

app_id = 'org.gnome.Chime'
chime_name = meson.project_name()

gnome = import('gnome')
i18n = import('i18n')

prefix = get_option('prefix')

bindir = join_paths(prefix, get_option('bindir'))
libdir = join_paths(prefix, get_option('libdir'))
datadir = join_paths(prefix, get_option('datadir'))

pkglibdir = join_paths(libdir, meson.project_name())
pkgdatadir = join_paths(datadir, meson.project_name())

desktopdir = join_paths(datadir, 'applications')
icondir = join_paths(datadir, 'icons')
metainfodir = join_paths(datadir, 'metainfo')
schemasdir = join_paths(datadir, 'glib-2.0', 'schemas')
servicedir = join_paths(datadir, 'dbus-1', 'services')
girdir = join_paths(pkgdatadir, 'gir-1.0')
typelibdir = join_paths(pkglibdir, 'girepository-1.0')

protoc = find_program('protoc-c')

desktop_file_validate = find_program('desktop-file-validate', required: false)
appstream_util = find_program('appstream-util', required: false)

gio = dependency('gio-2.0', version: '>= 2.43.4')
gtk3 = dependency('gtk+-3.0', version: '>= 3.21.6')
libsoup = dependency('libsoup-2.4')
json_glib = dependency('json-glib-1.0')
gstreamer = dependency('gstreamer-1.0')
gstreamer_app = dependency('gstreamer-app-1.0')
gstreamer_rtp = dependency('gstreamer-rtp-1.0')
gstreamer_video = dependency('gstreamer-video-1.0')
libprotobuf_c = dependency('libprotobuf-c')
libxml2 = dependency('libxml-2.0')
gnutls = dependency('gnutls', version: '>= 3.2.0')

root_dir = include_directories('.')

cdata = configuration_data()
cdata.set_quoted('LIBDIR', libdir)
cdata.set_quoted('DATADIR', datadir)
cdata.set_quoted('GETTEXT_PACKAGE', chime_name)
cdata.set_quoted('PACKAGE_VERSION', chime_version)
cdata.set_quoted('CHIME_CERTS_DIR', pkgdatadir)
configure_file(output: 'config.h', configuration: cdata)

subdir('protobuf')
subdir('chime')
subdir('src')
subdir('data')

meson.add_install_script('build-aux/meson_post_install.py')
