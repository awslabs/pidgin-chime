const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const GObject = imports.gi.GObject;
const Gtk = imports.gi.Gtk;
const Chime = imports.gi.Chime;
const ChimeUtils = imports.gi.ChimeUtils;

const RoomList = imports.roomList; // used in template
const RoomStack = imports.roomStack; // used in template
const UserList = imports.userList; // used in template


var FixedSizeFrame = GObject.registerClass({
    Properties: {
        height: GObject.ParamSpec.int('height',
                                      'height',
                                      'height',
                                      GObject.ParamFlags.READWRITE,
                                      -1, GLib.MAXINT32, -1),
        width: GObject.ParamSpec.int('width',
                                     'width',
                                     'width',
                                     GObject.ParamFlags.READWRITE,
                                     -1, GLib.MAXINT32, -1)
    },
}, class FixedSizeFrame extends Gtk.Frame {
    _init(params) {
        this._height = -1;
        this._width = -1;

        super._init(params);
    }

    _queueRedraw() {
        let child = this.get_child();
        if (child)
            child.queue_resize();
        this.queue_draw();
    }

    get height() {
        return this._height;
    }

    set height(height) {
        if (height == this._height)
            return;
        this._height = height;
        this.notify('height');
        this.set_size_request(this._width, this._height);
        this._queueRedraw();
    }

    get width() {
        return this._width;
    }

    set width(width) {
        if (width == this._width)
            return;

        this._width = width;
        this.notify('width');
        this.set_size_request(this._width, this._height);
        this._queueRedraw();
    }

    vfunc_get_preferred_width_for_height(forHeight) {
        let [min, nat] = super.vfunc_get_preferred_width_for_height(forHeight);
        return [min, this._width < 0 ? nat : this._width];
    }

    vfunc_get_preferred_height_for_width(forWidth) {
        let [min, nat] = super.vfunc_get_preferred_height_for_width(forWidth);
        return [min, this._height < 0 ? nat : this._height];
    }
});

var ConnectionViewer = GObject.registerClass({
    Template: 'resource:///org/gnome/Chime/ui/connection-viewer.ui',
    InternalChildren: ['stack',
                       'mainFrame',
                       'loginUsernameEntry',
                       'loginPasswordEntry',
                       'loginButton'],
}, class ConnectionViewer extends Gtk.Bin {
    _init(params) {
        super._init(params);

        this._settings = new Gio.Settings({ schema_id: 'org.gnome.Chime.Account' });

        this._loginUsernameEntry.connect('changed', this._onLoginUsernameEntryChanged.bind(this));
        this._loginPasswordEntry.connect('changed', this._onLoginPasswordEntryChanged.bind(this));
        this._loginButton.connect('clicked', this._onLoginButtonClicked.bind(this));
    }

    _updateLoginButtonSensitivity() {
        this._loginButton.sensitive = (this._loginUsernameEntry.text != '' && this._loginPasswordEntry.text != '');
    }

    _onLoginUsernameEntryChanged(entry) {
        this._updateLoginButtonSensitivity();
    }

    _onLoginPasswordEntryChanged(entry) {
        this._updateLoginButtonSensitivity();
    }

    _onLoginButtonClicked(widget) {
        this._username = this._loginUsernameEntry.text;
        if (this._username == '')
            return;

        this._devtoken = this._settings.get_string('devtoken');
        if (this._devtoken == "") {
            this._devtoken = ChimeUtils.util_generate_dev_token(this._username);
            log('dev token ' + this._devtoken);
        }

        log('connecting user ' + this._username);

        this._connection = new Chime.Connection({ account_email: this._username, device_token: this._devtoken });
        GObject.signal_connect(this._connection, 'authenticate', this._onConnectionAuthenticate.bind(this));
        GObject.signal_connect(this._connection, 'connected', this._onConnectionConnected.bind(this));
        GObject.signal_connect(this._connection, 'disconnected', this._onConnectionDisconnected.bind(this));
        GObject.signal_connect(this._connection, 'log-message', this._onConnectionLogMessage.bind(this));

        this._connection.connect();
    }

     _onConnectionAuthenticate(connection, user_required) {
        log('chime authenticate');
        let password = this._loginPasswordEntry.text;

        connection.authenticate(this._username, password);
    }

     _onConnectionConnected(connection, display_name) {
        log('chime connected as ' + display_name);
    }

     _onConnectionDisconnected(connection, err) {
        log('chime disconnected');
    }

    _onConnectionLogMessage(connection, level, str) {
        log('chime: ' + str);
    }
});
