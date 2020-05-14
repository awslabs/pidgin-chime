chime_sources = [
  'chime-call.c',
  'chime-call.h',
  'chime-call-screen.c',
  'chime-call-screen.h',
  'chime-call-transport.c',
  'chime-certs.c',
  'chime-connection.c',
  'chime-connection.h',
  'chime-connection-private.h',
  'chime-contact.c',
  'chime-contact.h',
  'chime-conversation.c',
  'chime-conversation.h',
  'chime-juggernaut.c',
  'chime-meeting.c',
  'chime-meeting.h',
  'chime-object.c',
  'chime-object.h',
  'chime-props.h',
  'chime-room.c',
  'chime-room.h',
  'chime-signin.c',
  'chime-websocket.c',
  'chime-websocket-connection.c',
  'chime-websocket-connection.h',
]

chime_sources_private = [
  'chime-call-audio.c',
  'chime-call-audio.h',

]

gen = generator(
  protoc,
  output    : ['@BASENAME@.pb-c.c', '@BASENAME@.pb-c.h'],
  arguments : [ '@INPUT@', '-I@SOURCE_DIR@/protobuf', '--c_out=@BUILD_DIR@']
)

proto_sources = gen.process(proto_files)

libargs = [
  '-DG_LOG_USE_STRUCTURED',
  '-DG_LOG_DOMAIN="Chime"'
]

chime_deps = [
  gio,
  gtk3,
  libsoup,
  json_glib,
  gstreamer,
  gstreamer_app,
  gstreamer_rtp,
  gstreamer_video,
  libprotobuf_c,
  libxml2,
  gnutls,
]

libchime = shared_library(
  'chime-1.0',
  [chime_sources, chime_sources_private, proto_sources],
  include_directories: [root_dir],
  dependencies: chime_deps,
  c_args: libargs,
  install: true,
  install_dir: pkglibdir
)

libchime_dep = declare_dependency(
  include_directories: include_directories('.'),
  dependencies: chime_deps,
  link_with: libchime
)

gnome.generate_gir(
  libchime,
  sources: chime_sources,
  nsversion: '1.0',
  namespace: 'Chime',
  symbol_prefix: 'chime',
  identifier_prefix: 'Chime',
  includes: ['Gio-2.0', 'Gtk-3.0'],
  extra_args: '--quiet',
  install_dir_gir: girdir,
  install_dir_typelib: typelibdir,
  install: true
)
