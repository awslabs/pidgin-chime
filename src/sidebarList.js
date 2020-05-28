const Gdk = imports.gi.Gdk;
const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const GObject = imports.gi.GObject;
const Gtk = imports.gi.Gtk;
const Chime = imports.gi.Chime;

var SidebarListRow = GObject.registerClass({
    Template: 'resource:///org/gnome/Chime/ui/sidebar-list-row.ui',
    InternalChildren: ['icon', 'label', 'counter'],
}, class SidebarListRow extends Gtk.ListBoxRow {
    _init(entity) {
        super._init();

        this._entity = entity;

        entity.bind_property('name', this._label, 'label',
                             GObject.BindingFlags.SYNC_CREATE);
    }

    get entity() {
        return this._entity;
    }
});

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

        this.set_header_func(this._updateHeader.bind(this));
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
        log('Sidebar: new room: ' + room.name);

        let row = new SidebarListRow(room);
        row.show();
        this.add(row);
    }

    _onConnectionNewConversation(connection, conversation) {
        log('New conversation: ' + conversation.name);

        let row = new SidebarListRow(conversation);
        row.show();
        this.add(row);
    }

    _updateHeader(row, before) {
        let getIsRoom = row => row ? row.entity instanceof Chime.Room : false;
        let getIsConversation = row => row ? row.entity instanceof Chime.Conversation : false;

        let oldHeader = row.get_header();

        if ((getIsRoom(before) && getIsRoom(row)) ||
            (getIsConversation(before) && getIsConversation(row))) {
            if (oldHeader)
                oldHeader.destroy();
            return;
        }

        if (oldHeader) {
            return;
        }

        let header = new Gtk.Label();
        header.show();

        if (getIsRoom(row)) {
            header.label = 'Chat rooms';
        } else {
            header.label = 'Recent messages';
        }

        row.set_header(header);
    }
});
