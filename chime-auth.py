#!/usr/bin/python3

import sys, dbus

if len(sys.argv) != 2:
    print("Need one argument: New Chime token")
    exit(1)

chimeurl = sys.argv[1]

if not chimeurl.startswith('chime://sso_sessions?Token='):
    print("Need Chime URL starting chime://sso_sessions?Token=")
    exit(1)

token = chimeurl[27:]

bus = dbus.SessionBus()

pidgin = bus.get_object("im.pidgin.purple.PurpleService",
                        "/im/pidgin/purple/PurpleObject")

accounts = pidgin.PurpleAccountsGetAllActive()

for account in accounts:
    proto = pidgin.PurpleAccountGetProtocolName(account)
    if proto != "Amazon Chime":
        continue
    token = pidgin.PurpleAccountSetString(account, "token", token)
    print(token)
    exit(0)


print("No Chime account found")
exit(1)
