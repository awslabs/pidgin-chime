const Gdk = imports.gi.Gdk;
const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const GObject = imports.gi.GObject;
const Gtk = imports.gi.Gtk;
const Chime = imports.gi.Chime;

const {AccountsMonitor} = imports.accountsMonitor;
const AppNotifications = imports.appNotifications;
const {MainWindow} = imports.mainWindow;
const {PasteManager} = imports.pasteManager;
const {RoomManager} = imports.roomManager;
const {UserStatusMonitor} = imports.userTracker;
const Utils = imports.utils;

const MAX_RETRIES = 3;

const IRC_SCHEMA_REGEX = /^(irc?:\/\/)([\da-z\.-]+):?(\d+)?\/(?:%23)?([\w\.\+-]+)/i;

var Application = GObject.registerClass({
    Signals: { 'prepare-shutdown': {},
               'room-focus-changed': {} },
}, class Application extends Gtk.Application {
    _init() {
        super._init({ application_id: 'org.gnome.Chime',
                      flags: Gio.ApplicationFlags.HANDLES_OPEN });

        GLib.set_application_name('Chime');
        GLib.set_prgname('org.gnome.Chime');
        this._retryData = new Map();
        this._nickTrackData = new Map();

        this._windowRemovedId = 0;

        this.add_main_option('version', 0,
                             GLib.OptionFlags.NONE, GLib.OptionArg.NONE,
                             _("Print version and exit"), null);
        this.connect('handle-local-options', (o, dict) => {
            let v = dict.lookup_value('version', null);
            if (v && v.get_boolean()) {
                print("Chime %s".format(pkg.version));
                return 0;
            }

            return -1;
        });
    }

    vfunc_startup() {
        super.vfunc_startup();

        let actionEntries = [
          { name: 'about',
            activate: this._onShowAbout.bind(this) },
          { name: 'quit',
            activate: this._onQuit.bind(this),
            accels: ['<Primary>q'] }
        ];
        actionEntries.forEach(actionEntry => {
            let props = {};
            ['name', 'state', 'parameter_type'].forEach(prop => {
                if (actionEntry[prop])
                    props[prop] = actionEntry[prop];
            });
            let action = new Gio.SimpleAction(props);
            if (actionEntry.create_hook)
                actionEntry.create_hook(action);
            if (actionEntry.activate)
                action.connect('activate', actionEntry.activate);
            if (actionEntry.change_state)
                action.connect('change-state', actionEntry.change_state);
            if (actionEntry.accels)
                this.set_accels_for_action('app.' + actionEntry.name,
                                           actionEntry.accels);
            this.add_action(action);
        });

        let provider = new Gtk.CssProvider();
        let uri = 'resource:///org/gnome/Chime/css/application.css';
        let file = Gio.File.new_for_uri(uri);
        try {
            provider.load_from_file(Gio.File.new_for_uri(uri));
        } catch(e) {
            logError(e, "Failed to add application style");
        }
        Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(),
                                                 provider,
                                                 Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    vfunc_activate() {
        if (!this.active_window) {
            let window = new MainWindow({ application: this });
        }

        this.active_window.present();
    }

    vfunc_open(files) {
        this.activate();
    }

    get isTestInstance() {
        return this.flags & Gio.ApplicationFlags.NON_UNIQUE;
    }

    _onShowAbout() {
        if (this._aboutDialog) {
            this._aboutDialog.present();
            return;
        }
        let aboutParams = {
            authors: [
                'Florian Müllner <fmuellner@gnome.org>',
                'William Jon McCann <william.jon.mccann@gmail.com>',
                'Carlos Soriano <carlos.soriano89@gmail.com>',
                'Giovanni Campagna <gcampagna@src.gnome.org>',
                'Carlos Garnacho <carlosg@gnome.org>',
                'Jonas Danielsson <jonas.danielsson@threetimestwo.org>',
                'Bastian Ilsø <bastianilso@gnome.org>',
                'Kunaal Jain <kunaalus@gmail.com>',
                'Cody Welsh <codyw@protonmail.com>',
                'Isabella Ribeiro <belinhacbr@gmail.com>',
                'Jonas Danielsson <jonas@threetimestwo.org>',
                'Rares Visalom <rares.visalom@gmail.com>',
                'Danny Mølgaard <moelgaard.dmp@gmail.com>',
                'Justyn Temme <Justyntemme@gmail.com>',
                'unkemptArc99 <abhishekbhardwaj540@gmail.com>'
            ],
            artists: [
                'Sam Hewitt <hewittsamuel@gmail.com>',
                'Jakub Steiner <jimmac@gmail.com>',
                'Lapo Calamandrei <calamandrei@gmail.com>'
            ],
            translator_credits: _("translator-credits"),
            comments: _("An Internet Relay Chat Client for GNOME"),
            copyright: 'Copyright © 2013-2018 The Chime authors',
            license_type: Gtk.License.GPL_2_0,
            logo_icon_name: 'org.gnome.Chime',
            version: pkg.version,
            website_label: _("Learn more about Chime"),
            website: 'https://wiki.gnome.org/Apps/Chime',

            transient_for: this.active_window,
            modal: true
        };

        this._aboutDialog = new Gtk.AboutDialog(aboutParams);
        this._aboutDialog.show();
        this._aboutDialog.connect('response', () => {
            this._aboutDialog.destroy();
            this._aboutDialog = null;
        });
    }

    _onQuit() {
        if (this._windowRemovedId)
            this.disconnect(this._windowRemovedId);
        this._windowRemovedId = 0;

        this.get_windows().reverse().forEach(w => { w.destroy(); });
        this.emit('prepare-shutdown');
    }
});
