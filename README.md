Pidgin plugin for Amazon Chime
==============================


Support
-------

This plugin is not supported by the Amazon Chime team. Do not contact them for
any assistance with this client.

It supports chat and meetings with audio.

You can join meetings using the [chime-joinable.py][chime-joinable.py]
script, e.g.:

    chime-joinable.py 123456768

There is a plugin for Evolution which allows Pidgin to spawn a new meeting
editor with the Chime meeting details prepopulated. Select 'Schedule
meeting…' from the account menu in Pidgin.

Todo
----

Video is not yet implemented, although it should actually be simple to
add since it is mostly just standard SRTP, unlike the audio and legacy
screenshare protocols which required a lot of extra work on the transport
stream.

Screen share used to work but the protocol has changed to treating it
like just another video stream.

We should extend the Evolution plugin so that it spots when meetings are
starting which contain a Chime link, but to which you weren't invited
directly so Chime doesn't auto-call. It should make those joinable
automatically, like the [chime-joinable.py][chime-joinable.py] script
does.


Installation
------------

    sudo dnf install 'pkgconfig(purple)' 'pkgconfig(libsoup-2.4)' 'pkgconfig(json-glib-1.0)'
    ./configure
    make
    sudo make install


Authentication
--------------

This plugin uses your system web browser by asking Pidgin to open a URI
for the login page. After a successful login, you end up redirected to a
URI like `chime://prpl?..."`. There is a desktop file which sets `chime:`
URIs to be handled by a [python script][chime-auth.py] which passes the
token back to Pidgin via D-Bus.


Debugging
---------

You can set the `CHIME_DEBUG` environment variable to get additional
debug output on top of the Pidgin debugging enabled with `-d`:

    CHIME_DEBUG=2 pidgin -d

[chime-auth.py]: chime-auth.py
[chime-joinable.py]: chime-joinable.py
