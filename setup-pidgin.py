#!/usr/bin/env python3
#
# Script to obtain an AWS Chime registration token.  No dependencies required,
# just a standard Python 3 installation.
#
# TODO: Add support for the "amazon" login provider
#
if __name__ != '__main__':
    raise RuntimeError("This module cannot be imported")

import gzip
import json
import os
import pwd
import re
import sys
import traceback
from argparse import ArgumentParser
from getpass import getpass
from html.parser import HTMLParser
from operator import itemgetter
from urllib import request as URL
from urllib.parse import urlsplit, urlunsplit, urljoin, urlencode, parse_qsl
from xml.etree import ElementTree as ET

# Constants that cannot be dynamically obtained (yet!)
SIGNING_ENDPOINT = "https://signin.id.ue1.app.chime.aws"
WARPDRIVE_ENTRYPOINT_FRAGMENT = "/WarpDriveLogin/"
WARPDRIVE_INTERFACE = "com.amazonaws.warpdrive.console.client.GalaxyInternalGWTService"
POLICY_PATH = "deferredjs/{}/5.cache.js"
GALAXY_PATH = "WarpDriveLogin/GalaxyInternalService"

def display(msg):
    print("\033[0;33m*** {}\033[0m".format(msg))

def error(msg, exc, debug=False):
    if debug:
        print("\033[0;31m!!! {}\033[0m".format(msg))
        traceback.print_exception(type(exc), exc, exc.__traceback__)
    else:
        print("\033[0;31m!!! {}:\033[0m {}".format(msg, exc))
    sys.exit(2)

# Poor man's replacement of requests.Session
class Session:
    def __init__(self, username, initial_url, debug=False):
        self.debug = debug
        self.client = URL.build_opener(URL.HTTPCookieProcessor)
        self.state = {
            'username': username,
            'starting_point': initial_url,
        }
        if debug:
            for h in self.client.handlers:
                if hasattr(h, 'set_http_debuglevel'):
                    h.set_http_debuglevel(1)

    #
    # Execute a step function (implemented below).  This is a weird design
    # choice to avoid having global variables, a complex object structure and
    # passing a dictionary around (explicitly).
    #
    def execute(self, step):
        try:
            display(step.__doc__)
            result = step(self, **self.state)
        except Exception as e:
            if self.debug:
                print()
            error("Step {} failed".format(step.__name__), e, self.debug)
        self.state.update(result)

    def request(self, *args):
        response = self.client.open(*args)
        assert response.getcode() == 200  # Just in case
        charset = response.getheader('content-type', "").partition("charset=")[2]
        body = response.read()
        if response.getheader('content-encoding', "").lower() == "gzip":
            body = gzip.decompress(body)
        response.body = body.decode(charset or 'latin1')
        if self.debug:
            print()
            print("-" * 72)
        return response

    #
    # Execute a GWT-RPC request.  This is code is based on the information found
    # in the following links:
    #
    # https://docs.google.com/document/d/1eG0YocsYYbNAtivkLtcaiEE5IOF5u4LUol8-LL0TIKU
    # http://www.gdssecurity.com/l/b/page/2/
    #
    def gwt_request(self, url, module_base, permutation, preamble, fields):
        last = 0
        strings = {}
        for f in preamble + fields:
            if f is not None and f not in strings:
                last += 1
                strings[str(f)] = last
        parts = ["7", "0", str(len(strings))]
        parts.extend(s for s, i in sorted(strings.items(), key=itemgetter(1)))
        strings[None] = 0
        parts.extend(str(strings[f]) for f in preamble)
        parts.append("1")  # Only one argument supported for now
        parts.extend(str(strings[f]) for f in fields)
        data = ("|".join(parts) + "|").encode("utf-8")
        response = self.request(URL.Request(url, data, {
            'Content-Type': "text/x-gwt-rpc; charset=utf-8",
            'X-GWT-Module-Base': module_base,
            'X-GWT-Permutation': permutation,
        }))
        if self.debug:
            print(response.body)
        assert response.body.startswith("//")
        status = response.body[2:4]
        wrapper = json.loads(response.body[4:])
        strings = wrapper[-3]
        strings.insert(0, None)
        typename, *values = [strings[int(i)] for i in reversed(wrapper[:-3])]
        if status == "EX":
            raise GWTException(*filter(None, values))
        return values

class GWTException(Exception):
    pass

#
# HTMLParser derived classes to inspect some HTML documents looking for concrete
# information.  This way, we avoid the risk of using unreliable regular
# expressions.
#
class HTMLGatherer(HTMLParser):
    def __init__(self):
        super().__init__()
        self.data = self.initial_data()

    def initial_data(self):
        raise NotImplementedError()

    @classmethod
    def parse(klass, text):
        instance = klass()
        instance.feed(text)
        instance.close()
        return instance.data
        
# Parser to get the CSRF token to lookup the @amazon.com domain
class ADSearchForm(HTMLGatherer):
    def initial_data(self):
        self.capture = False  # Ugh!
        return {
            'action': "",
            'method': "",
            'hidden_fields': {},
        }

    def handle_starttag(self, tag, attr_list):
        if tag == 'form':
            attrs = dict(attr_list)
            if attrs.get('id') == "picker_email":
                self.data['method'] = attrs['method'].upper()
                self.data['action'] = attrs['action']
                self.capture = True
        elif tag == 'input' and self.capture:
            attrs = dict(attr_list)
            if attrs['type'].lower() == "hidden":
                self.data['hidden_fields'][attrs['name']] = attrs['value']

    def handle_endtag(self, tag):
        if tag == 'form' and self.capture:
            self.capture = False

# Parser to find the GWT JS entry point of the AD login application
class GWTEntryPoint(HTMLGatherer):
    def initial_data(self):
        return {'url': None}

    def handle_starttag(self, tag, attr_list):
        if tag == 'script':
            attrs = dict(attr_list)
            if WARPDRIVE_ENTRYPOINT_FRAGMENT in attrs.get('src', ""):
                self.data['url'] = attrs['src']

#
# Now the login procedure steps.  They are expected to be invoked by
# Session.execute() since all the session state is passed as keyword arguments.
#
def find_directory(session, *, starting_point, username, **kw):
    """Finding the Active Directory name"""
    r = session.request(starting_point)
    form = ADSearchForm.parse(r.body)
    assert form['method'] == "POST"  # Just in case, for now
    form['hidden_fields']['profile[primary_email]'] = username + "@amazon.com"
    data = urlencode({k.encode('utf-8'): v.encode('utf-8')
                      for k, v in form['hidden_fields'].items()})
    action = urljoin(starting_point, form['action'])
    r = session.request(action, data.encode('ascii'))
    dir_info = json.loads(r.body)
    assert dir_info['provider'] == "wd"
    dir_info['login_url'] = urljoin(starting_point, dir_info['path'])
    dir_info.update(parse_qsl(urlsplit(dir_info['login_url']).query))
    return dir_info

def fetch_login_info(session, *, login_url, **kw):
    """Fetching Amazon AD login page"""
    r = session.request(login_url)
    location = urlsplit(r.geturl())
    login_info = dict(parse_qsl(location.query))  # client_id + redirect_uri
    login_info['gwt_entry'] = GWTEntryPoint.parse(r.body)['url']
    login_info['module_base'] = login_info['gwt_entry'].rsplit("/", 1)[0] + "/"
    login_info['gwt_rpc_url'] = urljoin(urlunsplit(location[:3] + ("", "")), GALAXY_PATH)
    return login_info

def gather_rpc_parameters(session, *, gwt_entry, module_base, **kw):
    """Discovering GWT-RPC necessary parameters"""
    id_finder = re.compile(r'''["']([A-Z0-9]{30,35})["']''')
    result = {}
    r = session.request(gwt_entry)
    match = id_finder.search(r.body)
    if match:
        result['permutation'] = match.group(1)
        deferred = urljoin(module_base, POLICY_PATH.format(match.group(1)))
        match = id_finder.search(session.request(deferred).body)
        if match:
            result['policy'] = match.group(1)
    return result

def get_region(session, *, gwt_rpc_url, module_base, permutation, policy,
               directory, client_id, redirect_uri, **kw):
    """Requesting the AWS region"""
    preamble = (module_base, policy, WARPDRIVE_INTERFACE, "validateClient")
    type_name = "com.amazonaws.warpdrive.console.shared.ValidateClientRequest_v2/2136236667"
    fields = (type_name, type_name, "ONFAILURE", client_id, directory, None,
              None, redirect_uri)
    r = session.gwt_request(gwt_rpc_url, module_base, permutation, preamble, fields)
    return {'region': r[-1]}

def get_auth_code(session, *, gwt_rpc_url, module_base, permutation, policy,
                  directory, client_id, username, password, **kw):
    """Authenticating"""
    preamble = (module_base, policy, WARPDRIVE_INTERFACE, "authenticateUser")
    type_name = "com.amazonaws.warpdrive.console.shared.LoginRequest_v4/3859384737"
    fields = (type_name, type_name, "", "", client_id, "", None, directory,
              password, "", username)
    r = session.gwt_request(gwt_rpc_url, module_base, permutation, preamble, fields)
    return {'auth_code': r[1]}

def get_token(session, *, directory, region, auth_code, redirect_uri, **kw):
    """Obtaining the AWS Chime registration token"""
    token_finder = re.compile(r'''["']chime://sso_sessions\?Token=([^"']+)["']''')
    data = urlencode({
        'organization': directory,
        'region': region,
        'auth_code': auth_code,
    })
    result = {}
    callback = urlunsplit(urlsplit(redirect_uri)[:3] + (data, ""))
    r = session.request(callback)
    match = token_finder.search(r.body)
    if match:
        result['token'] = match.group(1)
    return result

def save_token(session, *, force, username, starting_point, token, accounts_file, **kw):
    """Saving libpurple accounts configuration"""
    if os.path.exists(accounts_file):
        document = ET.parse(accounts_file)
    else:
        document = ET.ElementTree(ET.Element('account', {'version': "1.0"}))
    account = None
    for a in document.findall("./account"):
        p = a.find("./protocol")
        if getattr(p, 'text', "") == "prpl-chime":
            account = a
            break
    if account:
        if not force:
            raise Exception("Existing AWS Chime account found, use --force to overwrite")
        name = account.find("./name")
        if name is None:
            ET.SubElement(account, 'name').text = username
        else:
            name.text = username
        settings = account.find("./settings")
        byname = {s.get('name'): s for s in settings.findall("./setting")}
        for n, v in (('server', starting_point), ('token', token)):
            if n in byname:
                byname[n].text = v
            else:
                ET.SubElement(settings, 'setting', name=n, type="string").text = v
    else:
        account = ET.Element('account')
        ET.SubElement(account, 'protocol').text = "prpl-chime"
        ET.SubElement(account, 'name').text = username
        settings = ET.SubElement(account, 'settings')
        ET.SubElement(settings, 'setting', name="server", type="string").text = starting_point
        ET.SubElement(settings, 'setting', name="token", type="string").text = token
        document.getroot().append(account)
    document.write(accounts_file, encoding='UTF-8', xml_declaration=True)
    return {}

def main():
    whoami = pwd.getpwuid(os.geteuid()).pw_name
    parser = ArgumentParser(description="Obtain an AWS Chime registration token and add it to the account configuration.")
    parser.add_argument("--user", "-u", default=whoami, metavar='USERNAME',
                        help="define authentication user (default {})".format(whoami))
    parser.add_argument("--config", "-c", default="~/.purple", metavar='DIR',
                        help="libpurple configuration directory (default ~/.purple)")
    parser.add_argument("--endpoint", default=SIGNING_ENDPOINT, metavar='URL',
                        help="use different signing endpoint")
    parser.add_argument("--dry-run", "-n", action='store_true',
                        help="obtain the token and print it, but don't save it")
    parser.add_argument("--force", "-f", action='store_true',
                        help="save token unconditionally (i.e. replace existing)")
    parser.add_argument("--debug", "-d", action='store_true',
                        help="show HTTP traffic and tracebacks")
    args = parser.parse_args()
    session = Session(args.user, args.endpoint, debug=args.debug)
    session.execute(find_directory)
    session.execute(fetch_login_info)
    session.execute(gather_rpc_parameters)
    session.execute(get_region)
    session.state['password'] = getpass("Enter password for {}: ".format(args.user))
    session.execute(get_auth_code)
    session.execute(get_token)
    if args.dry_run:
        print()
        print(session.state['token'])
    else:
        session.state['force'] = args.force
        session.state['accounts_file'] = os.path.join(os.path.expanduser(args.config),
                                                      "accounts.xml")
        session.execute(save_token)

main()
