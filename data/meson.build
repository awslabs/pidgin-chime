subdir('icons')

desktop_file = app_id + '.desktop'
i18n.merge_file(desktop_file,
  input: desktop_file + '.in',
  output: desktop_file,
  po_dir: '../po',
  install: true,
  install_dir: desktopdir,
  type: 'desktop'
)

if (desktop_file_validate.found())
  test('Validating ' + desktop_file,
    desktop_file_validate,
    args: [desktop_file],
    workdir: meson.current_build_dir()
  )
endif

gnome.compile_schemas(build_by_default: true)

install_data(
  app_id + '.gschema.xml',
  install_dir: schemasdir
)
