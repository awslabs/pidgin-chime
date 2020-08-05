app_sources = files(
  'application.c',
  'application.h',
  'connectionviewer.c',
  'connectionviewer.h',
  'dirs.c',
  'dirs.h',
  'main.c',
  'meetinglistboxrow.c',
  'meetinglistboxrow.h',
  'meetingsview.c',
  'meetingsview.h',
  'window.c',
  'window.h',
)

app_deps = [
  gio,
  libsoup,
  libprotobuf_c,
  gtk3,
  libchime_dep
]

resources = gnome.compile_resources(
  'chime-resources',
  'resources/chime.gresource.xml',
  source_dir: 'resources',
  export: true
)

sources = [
  app_sources,
  resources,
]

executable(
  chime_name,
  sources: sources,
  include_directories: [root_dir],
  dependencies: app_deps,
  c_args: [
    '-DG_LOG_DOMAIN="Chime"'
  ],
  install: true,
  install_dir: bindir,
)
