const Gdk = imports.gi.Gdk;
const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const GObject = imports.gi.GObject;
const Gtk = imports.gi.Gtk;
const Chime = imports.gi.Chime;


var SidebarList = GObject.registerClass({
    Properties: {
        connection: GObject.ParamSpec.object('connection',
                                             'connection',
                                             'connection',
                                             GObject.ParamFlags.READWRITE,
                                             Chime.Connection.$gtype)
    },
}, class SidebarList extends Gtk.ListBox {
    _init(params) {
        super._init(params);

        log('New sidebar list');

        //this.set_header_func(this._updateHeader.bind(this));
        //this.set_sort_func(this._sort.bind(this));

        this._roomRows = new Map();
    }

    get connection() {
        return this._connection;
    }

    set connection(c) {
        if (c == this._connection)
            return;

        this._connection = c;

        this.notify('connection');

        if (!this._connection)
            return; // finished

        GObject.signal_connect(this._connection, 'new-room', this._onConnectionNewRoom.bind(this));
        GObject.signal_connect(this._connection, 'new-conversation', this._onConnectionNewConversation.bind(this));
    }

    _onConnectionNewRoom(connection, room) {
        log('Sidebar: new room: ' + room.get_name());
        let row = new Gtk.ListBoxRow();
        row.add(new Gtk.Label({ label: room.get_name() }));
        row.show_all();

        this.add(row);
    }

    _onConnectionNewConversation(connection, conversation) {
        log('New conversation: ' + conversation.get_name());
    }
});
