"""Use WorkBuddy's official WeChat webview handoff for desktop authorization."""
import json
import subprocess
import sys
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import Request, urlopen

from gi.repository import GLib

import wechat_links


def open_page(url):
    subprocess.run([sys.executable, '/usr/lib/arlinux/guest/browser.py', url], check=True)


def main(url):
    target = urlsplit(url)
    query = parse_qs(target.query)
    if (target.scheme != 'https' or target.netloc != 'www.workbuddy.cn'
            or target.path.rstrip('/') != '/login'
            or query.get('platform') != ['workbuddy'] or not query.get('state')
            or not wechat_links.available()):
        open_page(url)
        return
    try:
        # This is the same mini-program webview used by the official login
        # page. Preserve the client's complete URL, including its session/state;
        # opening the mini-program home page cannot authorize that client.
        data = json.dumps(dict(path='/subpackages/secondary/webview/index',
                               query=urlencode({'url': url}), expire_days=1)).encode()
        request = Request('https://www.workbuddy.cn/v2/as/wechatmp/urllink/generate',
                          data=data, headers={'Content-Type': 'application/json'})
        with urlopen(request, timeout=10) as response:
            link = json.loads(response.read(1024 * 1024))['url_link']
        address = urlsplit(link)
        if address.scheme != 'https' or address.netloc != 'wxmpurl.cn':
            raise ValueError('Unexpected WeChat handoff origin')
        wechat_links.open_link(link)
        print('WorkBuddy: desktop authorization opened in Linux WeChat.', flush=True)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, GLib.Error):
        # The provider can change its web flow, or WeChat may be unavailable.
        # Keep the ordinary QR/phone/email login usable; never log auth URLs.
        print('WorkBuddy: WeChat handoff unavailable; opening browser sign-in.', file=sys.stderr)
        open_page(url)


if __name__ == '__main__':
    main(sys.argv[1])
