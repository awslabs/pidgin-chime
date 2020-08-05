#!/usr/bin/env python3

import os
import subprocess
import platform

prefix = os.environ.get('MESON_INSTALL_PREFIX', '/usr/local')
datadir = os.path.join(prefix, 'share')

# Packaging tools define DESTDIR and this isn't needed for them
if 'DESTDIR' not in os.environ:
    print('Compiling GSettings schemas...')
    subprocess.call(['glib-compile-schemas',
                    os.path.join(datadir, 'glib-2.0', 'schemas')])

    print('Updating icon cache...')
    try:
        subprocess.call(['gtk3-update-icon-cache', '-qtf',
                        os.path.join(datadir, 'icons', 'hicolor')])
    except:
        subprocess.call(['gtk-update-icon-cache', '-qtf',
                        os.path.join(datadir, 'icons', 'hicolor')])

    if platform.system() == 'Linux':
        print('Updating desktop database...')
        subprocess.call(['update-desktop-database', '-q',
                        os.path.join(datadir, 'applications')])

        print('Updating mime database...')
        subprocess.call(['update-mime-database',
                        os.path.join(datadir, 'mime')])