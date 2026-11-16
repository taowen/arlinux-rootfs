"""Open WeChat application links and official URL Links in the Linux handler."""
import re
from urllib.parse import parse_qs, urlsplit
from urllib.request import urlopen

from gi.repository import Gio


def available():
    return Gio.AppInfo.get_default_for_uri_scheme('weixin') is not None


def open_link(url):
    handler = Gio.AppInfo.get_default_for_uri_scheme('weixin')
    if handler is None:
        raise RuntimeError('No Linux application handles WeChat links')
    address = urlsplit(url)
    if address.scheme == 'https' and address.netloc == 'wxmpurl.cn':
        # The desktop landing page does not navigate automatically. Use only
        # its server-issued business ticket, never synthesize one or evaluate JS.
        with urlopen(url, timeout=10) as response:
            if urlsplit(response.url).netloc != 'wxmpurl.cn':
                raise ValueError('Unexpected WeChat landing origin')
            page = response.read(1024 * 1024).decode('utf-8')
        match = re.search(r"url_scheme:\s*'([^'\\]+)'", page)
        url = match[1] if match else ''
        address = urlsplit(url)
        if (address.scheme != 'weixin' or address.netloc != 'dl'
                or address.path != '/business/' or not parse_qs(address.query).get('t')):
            raise ValueError('WeChat landing page did not provide an application link')
    elif address.scheme != 'weixin':
        raise ValueError('Unsupported WeChat link')
    if not handler.launch_uris([url], None):
        raise RuntimeError('Linux WeChat did not accept the application link')
