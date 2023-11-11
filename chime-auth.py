#!/usr/bin/python3

import sys, dbus

if len(sys.argv) != 2:
    print("Need one argument: New Chime token")
    exit(1)

chimeurl = sys.argv[1]

if not chimeurl.startswith('chime://prpl?'):
    print("Need Chime URL starting chime://prpl?")
    exit(1)

devtoken = None
token = None
prpl_ui = None

query = chimeurl[13:].split("&")
for q in query:
    fields = q.split("=")
    if fields[0] == "devtoken":
        devtoken = fields[1]
    elif fields[0] == "Token":
        token = fields[1]
    elif fields[0] == "prpl_ui":
        prpl_ui = fields[1]
    else:
        print("Unknown query '%s'" % q);

if not token:
    print("token not found")
    exit(1)

if not devtoken:
    print("devtoken not found")
    exit(1)

bus = dbus.SessionBus()

pidgin = bus.get_object("im.pidgin.purple.PurpleService",
                        "/im/pidgin/purple/PurpleObject")

accounts = pidgin.PurpleAccountsGetAll()

for account in accounts:
    proto = pidgin.PurpleAccountGetProtocolName(account)
    if proto != "Amazon Chime":
        continue

    acct_devtoken = pidgin.PurpleAccountGetString(account, "devtoken", "")
    if devtoken != acct_devtoken:
        continue

    token = pidgin.PurpleAccountSetString(account, "token", token)
    pidgin.PurpleAccountClearCurrentError(account)
    if prpl_ui:
        pidgin.PurpleAccountSetEnabled(account, prpl_ui, 1)
    exit(0)


print("No Chime account found with devtoken %s" % devtoken)
exit(1)
