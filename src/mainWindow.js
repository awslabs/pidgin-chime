const Gdk = imports.gi.Gdk;
const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const GObject = imports.gi.GObject;
const Gtk = imports.gi.Gtk;
const Mainloop = imports.mainloop;
const Chime = imports.gi.Chime;

const {AccountsMonitor} = imports.accountsMonitor;
const {JoinDialog} = imports.joinDialog;
const {RoomManager} = imports.roomManager;
const {ConnectionViewer} = imports.connectionViewer; // used in template
const Utils = imports.utils;


var MainWindow = GObject.registerClass({
    Template: 'resource:///org/gnome/Chime/ui/main-window.ui',
    InternalChildren: ['titlebarRight',
                       'titlebarLeft',
                       'joinButton',
                       'showUserListButton',
                       'userListPopover',
                       'closeConfirmationDialog'],
    Properties: {
        subtitle: GObject.ParamSpec.string('subtitle',
                                           'subtitle',
                                           'subtitle',
                                           GObject.ParamFlags.READABLE,
                                           ''),
        'subtitle-visible': GObject.ParamSpec.boolean('subtitle-visible',
                                                      'subtitle-visible',
                                                      'subtitle-visible',
                                                      GObject.ParamFlags.READABLE,
                                                      false),
        'active-room': GObject.ParamSpec.object('active-room',
                                                'active-room',
                                                'active-room',
                                                GObject.ParamFlags.READWRITE,
                                                Chime.Room.$gtype)
    },
    Signals: { 'active-room-state-changed': {} },
}, class MainWindow extends Gtk.ApplicationWindow {
    _init(params) {
        this._subtitle = '';
        params.show_menubar = false;

        this._room = null;
        this._lastActiveRoom = null;

        this._displayNameChangedId = 0;
        this._topicChangedId = 0;
        this._membersChangedId = 0;
        this._channelChangedId = 0;

        super._init(params);

        this._settings = new Gio.Settings({ schema_id: 'org.gnome.Chime' });
        this._gtkSettings = Gtk.Settings.get_default();

        this._currentSize = [-1, -1];
        this._isMaximized = false;
        this._isFullscreen = false;

        this.connect('window-state-event', this._onWindowStateEvent.bind(this));
        this.connect('size-allocate', this._onSizeAllocate.bind(this));
        this.connect('destroy', this._onDestroy.bind(this));
        this.connect('delete-event', this._onDeleteEvent.bind(this));

        let size = this._settings.get_value('window-size').deep_unpack();
        if (size.length == 2)
            this.set_default_size.apply(this, size);

        if (this._settings.get_boolean('window-maximized'))
            this.maximize();
    }

    get subtitle() {
        return this._subtitle;
    }

    get subtitle_visible() {
        return this._subtitle.length > 0;
    }

    _onWindowStateEvent(widget, event) {
        let state = event.get_window().get_state();

        this._isFullscreen = (state & Gdk.WindowState.FULLSCREEN) != 0;
        this._isMaximized = (state & Gdk.WindowState.MAXIMIZED) != 0;
    }

    _onSizeAllocate(widget, allocation) {
        if (!this._isFullscreen && !this._isMaximized)
            this._currentSize = this.get_size();
    }

    _onDestroy(widget) {
        this._settings.set_boolean ('window-maximized', this._isMaximized);
        this._settings.set_value('window-size',
                                 GLib.Variant.new('ai', this._currentSize));

        let serializedChannel = null;
        if (this._lastActiveRoom)
            serializedChannel = new GLib.Variant('a{sv}', {
                account: new GLib.Variant('s', this._lastActiveRoom.account.object_path),
                channel: new GLib.Variant('s', this._lastActiveRoom.channel_name)
            });

        if (serializedChannel)
            this._settings.set_value('last-selected-channel', serializedChannel);
        else
            this._settings.reset('last-selected-channel');

        this.active_room = null;
    }

    _touchFile(file) {
        try {
            file.get_parent().make_directory_with_parents(null);
        } catch(e if e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.EXISTS)) {
            // not an error, carry on
        }

        let stream = file.create(0, null);
        stream.close(null);
    }

    _onDeleteEvent() {
        let f = Gio.File.new_for_path(GLib.get_user_cache_dir() +
                                      '/chime/close-confirmation-shown');
        try {
            this._touchFile(f);
        } catch(e) {
            if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.EXISTS))
                return Gdk.EVENT_PROPAGATE; // the dialog has been shown
            log('Failed to mark confirmation dialog as shown: ' + e.message);
        }

        this._closeConfirmationDialog.show();
        return Gdk.EVENT_STOP;
    }

    _updateDecorations() {
        let layoutLeft = null;
        let layoutRight = null;

        let layout = this._gtkSettings.gtk_decoration_layout;
        if (layout) {
            let split = layout.split(':');

            layoutLeft = split[0] + ':';
            layoutRight = ':' + split[1];
        }

        this._titlebarLeft.set_decoration_layout(layoutLeft);
        this._titlebarRight.set_decoration_layout(layoutRight);
    }

    get active_room() {
        return this._room;
    }

    set active_room(room) {
        if (room == this._room)
            return;

        if (this._room) {
            this._room.disconnect(this._displayNameChangedId);
            this._room.disconnect(this._topicChangedId);
            this._room.disconnect(this._membersChangedId);
            this._room.disconnect(this._channelChangedId);
        }
        this._displayNameChangedId = 0;
        this._topicChangedId = 0;
        this._membersChangedId = 0;
        this._channelChangedId = 0;

        if (room && room.type == Tp.HandleType.ROOM)
            this._lastActiveRoom = room;
        this._room = room;

        this._updateTitlebar();

        this.notify('active-room');
        this.emit('active-room-state-changed');

        if (!this._room)
            return; // finished
    }

    showJoinRoomDialog() {
        let dialog = new JoinDialog({ transient_for: this });
        dialog.show();
    }

    _updateTitlebar() {
        let subtitle = '';
        if (this._room && this._room.topic) {
            let urls = Utils.findUrls(this._room.topic);
            let pos = 0;
            for (let i = 0; i < urls.length; i++) {
                let url = urls[i];
                let text = this._room.topic.substr(pos, url.pos - pos);
                let urlText = GLib.markup_escape_text(url.url, -1);
                subtitle += GLib.markup_escape_text(text, -1) +
                            '<a href="%s">%s</a>'.format(urlText, urlText);
                pos = url.pos + url.url.length;
            }
            subtitle += GLib.markup_escape_text(this._room.topic.substr(pos), -1);
        }

        if (this._subtitle != subtitle) {
            this._subtitle = subtitle;
            this.notify('subtitle');
            this.notify('subtitle-visible');
        }

        this.title = this._room ? this._room.display_name : null;
    }
});
