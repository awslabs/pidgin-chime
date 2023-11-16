Pidgin plugin for Amazon Chime
==============================


Support
-------

This plugin is not supported by the Amazon Chime team. Do not contact them for
any assistance with this client.


Installation
------------

    sudo dnf install 'pkgconfig(purple)' 'pkgconfig(libsoup-2.4)' 'pkgconfig(json-glib-1.0)'
    ./configure
    make
    sudo make install

Given the rate of development, you may find it easier to make a symbolic link
from the installed plugin (e.g. `/usr/lib64/purple-2/libchimeprpl.so`) to
`.libs/libchimeprpl.so` in your working tree.


Authentication
--------------

This plugin is capable of obtaining the session token by emulating a web browser
and following the sign in process.  Currently, it is possible to log in with
your corporate credentials (Active Directory) as well as with your Amazon
credentials.

During this process, user input may be required (user and password, or just
password).  Make sure you are using a *libpurple* application that properly
implements interactive user input.

Passwords will **not** be stored anywhere.  Passwords are only necessary to
obtain a session token, and new session tokens are obtained with the previous
one.  If the token gets lost or corrupted, the sign in process will be triggered
again to obtain a new token.

In case all this stops working, the session token can be obtained with a web
browser; but first report this situation.  Start from [this link][signin] and
complete the authentication process until you end up at a URI that the browser
cannot handle, which looks something like

    chime://sso_sessions?Token=eyJyZ…

The part after `Token=` is your authentication token. Create an account in
Pidgin, select *Amazon Chime* as the protocol in the *Basic* tab then go to the
*Advanced* tab and paste the token in the *Token* field.  Leave the *Signin URL*
field empty.


Debugging
---------

Run from a terminal with the `CHIME_DEBUG` environment variable set to a
non-empty string.

This repository also includes a command specifically intended to ease debugging
the sign in web scraping code.  It's not compiled by default.  In order to
build it and run it, use the following commands:

    make chime-get-token
    CHIME_DEBUG=2 ./chime-get-token my-login-address@example.com

This will dump all the HTTP request performed during the authentication and
token retrieval.  If possible, attach its output when reporting an
authentication issue.


[signin]: https://signin.id.ue1.app.chime.aws/
