#!/usr/bin/python3

import sys, dbus

if len(sys.argv) != 2:
    print("Need one argument: PIN of meeting to join")
    exit(1)


bus = dbus.SessionBus()

pidgin = bus.get_object("im.pidgin.purple.PurpleService",
                        "/im/pidgin/purple/PurpleObject")

accounts = pidgin.PurpleAccountsGetAllActive()

for account in accounts:
    proto = pidgin.PurpleAccountGetProtocolName(account)
    if proto != "Amazon Chime":
        continue
    pidgin.ChimeAddJoinableMeeting(account, sys.argv[1])
    exit(0)


print("No Chime account found")
exit(1)
