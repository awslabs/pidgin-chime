data_resources = gnome.compile_resources(
  'data-resources',
  '../data/@0@.data.gresource.xml'.format(app_id),
  source_dir: '../data',
  c_name: 'data_resources'
)

js_sources = [
  'accountsMonitor.js',
  'application.js',
  'appNotifications.js',
  'chatView.js',
  'emojiPicker.js',
  'entryArea.js',
  'joinDialog.js',
  'main.js',
  'mainWindow.js',
  'pasteManager.js',
  'roomList.js',
  'roomManager.js',
  'roomStack.js',
  'tabCompletion.js',
  'userList.js',
  'utils.js',
  'userTracker.js'
]

js_xml = []
foreach js_source : js_sources
  js_xml += '    <file>@0@</file>'.format(js_source)

  if (js52.found())
    test('Checking syntax of ' + js_source, js52,
      args: ['-s', '-c', js_source],
      workdir: meson.current_source_dir()
    )
  endif
endforeach

resource_data = configuration_data()
resource_data.set('JS_SOURCE_FILES', '\n'.join(js_xml))

resource_xml_name = '@0@.src.gresource.xml'.format(app_id)
src_resources_xml = configure_file(
  input: resource_xml_name + '.meson',
  output: resource_xml_name,
  configuration: resource_data
)

src_resources = gnome.compile_resources(
  'src-resources',
  src_resources_xml,
  c_name: 'src_resources'
)

exeargs = [
  '-DPACKAGE_NAME="chime"',
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DPREFIX="@0@"'.format(prefix),
  '-DLIBDIR="@0@"'.format(libdir),
  '-DPKGLIBDIR="@0@"'.format(pkglibdir)
]
chime = executable('chime', ['chime.c', src_resources, data_resources],
  dependencies: [gio, girepository, gjs],
  c_args: exeargs,
  install: true
)

libsources = [
  'lib/chime-client-factory.c',
  'lib/chime-client-factory.h',
  'lib/chime-drag-helper.c',
  'lib/chime-drag-helper.h',
  'lib/chime-room.c',
  'lib/chime-room.h',
  'lib/chime-util.c',
  'lib/chime-util.h'
]

lib_nongir_sources = [
  'lib/chime-tp-autocleanup.h',
]

libargs = [
  '-DG_LOG_USE_STRUCTURED',
  '-DG_LOG_DOMAIN="ChimeUtils"'
]
libchimeutils = shared_library('chime-utils-1.0', libsources + lib_nongir_sources,
  dependencies: [gio, gtk3, telepathy_glib],
  c_args: libargs,
  install: true,
  install_dir: pkglibdir
)

libchimeutils_dep = declare_dependency(
  include_directories: include_directories('lib'),
  dependencies: [gio, gtk3, telepathy_glib],
  link_with: libchimeutils
)

gnome.generate_gir(libchimeutils,
  sources: libsources,
  nsversion: '1.0',
  namespace: 'ChimeUtils',
  symbol_prefix: 'chime',
  identifier_prefix: 'Chime',
  includes: ['Gio-2.0', 'Gtk-3.0', 'TelepathyGLib-0.12'],
  extra_args: '--quiet',
  install_dir_gir: girdir,
  install_dir_typelib: typelibdir,
  install: true
)

subdir('tests')
