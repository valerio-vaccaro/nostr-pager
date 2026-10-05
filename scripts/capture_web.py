#!/usr/bin/env python3
"""Capture an existing pager from a terminal with LAN access; never mutate pager data."""
import argparse
from datetime import datetime, timezone
import getpass
import json
from pathlib import Path
import re
import shutil
import sys
from urllib.parse import urlencode, urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='https://192.168.8.144', help='Pager HTTPS origin')
    parser.add_argument('--output', type=Path, help='Output directory (default: timestamped test-results/web folder)')
    parser.add_argument('--chrome', help='Chrome/Chromium executable; otherwise use installed Chrome or Playwright Chromium')
    args = parser.parse_args()
    origin = args.url.rstrip('/')
    parts = urlsplit(origin)
    if parts.scheme != 'https' or not parts.hostname or parts.username or parts.password or parts.path or parts.query or parts.fragment:
        parser.error('--url must be an HTTPS origin, for example https://192.168.8.144')
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print('Install the capture dependency: .venv/bin/python -m pip install playwright', file=sys.stderr)
        return 1

    root = Path(__file__).resolve().parents[1]
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = (args.output or root / 'test-results' / 'web' / stamp).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {'captured_at': stamp, 'origin': origin, 'pages': [], 'errors': [], 'skipped': [],
              'note': 'Live browser captures. Screenshots may contain displayed messages and contacts. No PIN, cookies or raw API state saved.'}
    report_path = output / 'report.json'

    def save_report():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    save_report()
    pin = getpass.getpass('Current six-digit display PIN: ').strip()
    if not re.fullmatch(r'[0-9]{6}', pin):
        report['errors'].append('PIN must contain six digits; no login attempted.')
        save_report()
        print(report['errors'][-1], file=sys.stderr)
        return 1

    def error_text(error):
        return str(error).replace(pin_secret, '[PIN REDACTED]').replace(origin, '[pager]')[:1000]

    pin_secret = pin
    executable = args.chrome or shutil.which('google-chrome') or shutil.which('chromium') or shutil.which('chromium-browser')
    current = 'browser startup'
    try:
        with sync_playwright() as playwright:
            options = {'headless': True}
            if executable:
                options['executable_path'] = executable
            browser = playwright.chromium.launch(**options)
            # The pager uses a self-signed certificate. This isolated session
            # does not change the user's browser trust store or existing login.
            context = browser.new_context(ignore_https_errors=True, viewport={'width': 1280, 'height': 900}, device_scale_factor=1)
            page = context.new_page()
            page.set_default_timeout(30000)
            page.set_default_navigation_timeout(45000)

            def allowed(route):
                request = route.request
                target = urlsplit(request.url)
                same_origin = (target.scheme, target.netloc) == (parts.scheme, parts.netloc)
                read = request.method in ('GET', 'HEAD')
                session = request.method == 'POST' and target.path in ('/login', '/logout')
                if same_origin and (read or session):
                    route.continue_()
                else:
                    route.abort()

            context.route('**/*', allowed)

            def visit(path):
                response = page.goto(origin + path, wait_until='load')
                if response is None or response.status != 200:
                    raise RuntimeError(f'HTTP {response.status if response else "no response"}')
                if path != '/login' and urlsplit(page.url).path == '/login':
                    raise RuntimeError('Login expired; rerun with the current display PIN.')
                if path.startswith('/messages') and '?' not in path:
                    page.wait_for_function("document.querySelector('#messages')?.children.length > 0")
                page.evaluate('document.fonts.ready')
                return response.status

            def screenshot(name, path, navigate=True):
                nonlocal current
                current = name
                status = visit(path) if navigate else 200
                # Never include filled credential fields in an image.
                masks = page.locator('input[type=password], input[name=key], input[name=secret]')
                page.screenshot(path=str(output / (name + '.png')), full_page=True, animations='disabled', mask=[masks])
                report['pages'].append({'name': name, 'path': urlsplit(path).path, 'http_status': status,
                                        'file': name + '.png', 'viewport': page.viewport_size})
                save_report()
                print('Captured ' + name, flush=True)

            try:
                screenshot('login', '/login')
                current = 'login submission'
                page.locator('input[name=pin]').fill(pin)
                pin = ''
                with page.expect_navigation(wait_until='load') as navigation:
                    page.locator('form[action="/login"] button').click()
                result = navigation.value
                if result is None or result.status != 200 or urlsplit(page.url).path != '/messages':
                    code = result.status if result else 'no response'
                    raise RuntimeError(f'Login failed (HTTP {code}); check the current PIN. No automatic login retries.')
                page.wait_for_function("document.querySelector('#messages')?.children.length > 0")
                screenshot('inbox', '/messages', navigate=False)
                current = 'state inspection'
                state = page.evaluate("""async () => {
                    const controller = new AbortController();
                    const timer = setTimeout(() => controller.abort(), 15000);
                    try {
                        const r = await fetch('/api/state', {cache:'no-store', signal:controller.signal});
                        if (!r.ok) throw new Error('State HTTP ' + r.status);
                        return await r.json();
                    } finally { clearTimeout(timer); }
                }""")
                report['firmware'] = state.get('version')
                report['can_send'] = state.get('canSend')
                report['message_count'] = len(state.get('messages', []))
                report['contact_count'] = len(state.get('contacts', []))
                pages = [('compose', '/compose'), ('contacts', '/contacts'), ('relays', '/relays'),
                         ('settings', '/settings'), ('setup', '/setup'),
                         ('delete-all-confirm', '/messages/delete/confirm?all=yes'),
                         ('restart-confirm', '/reset/confirm?mode=restart'),
                         ('factory-reset-confirm', '/reset/confirm?mode=erase')]
                messages = state.get('messages', [])
                if messages:
                    query = urlencode({'id': messages[0]['id']})
                    pages += [('message', '/message?' + query),
                              ('reply', '/compose?' + urlencode({'reply': messages[0]['id']})),
                              ('delete-message-confirm', '/messages/delete/confirm?' + query)]
                else:
                    report['skipped'].append('Reader, reply and single-message confirmation: inbox is empty.')
                # Keep one browser page open to respect the two-socket HTTPS limit.
                for name, path in pages:
                    try:
                        screenshot(name, path)
                    except Exception as error:
                        report['errors'].append({'page': name, 'error': error_text(error)})
                        save_report()
                        print(f'Could not capture {name}; continuing. See report.json.', file=sys.stderr)
                current = 'contact editor'
                try:
                    visit('/contacts')
                    page.locator('#contact-editor').evaluate('(el) => { el.open = true; }')
                    screenshot('add-contact', '/contacts', navigate=False)
                except Exception as error:
                    report['errors'].append({'page': current, 'error': error_text(error)})
                page.set_viewport_size({'width': 390, 'height': 844})
                for name, path in [('inbox-mobile', '/messages'), ('compose-mobile', '/compose')]:
                    try:
                        screenshot(name, path)
                    except Exception as error:
                        report['errors'].append({'page': name, 'error': error_text(error)})
            finally:
                # Release our session slot when possible; never save cookies.
                try:
                    report['logged_out'] = page.evaluate("""async () => {
                        const token = document.querySelector('form[action="/logout"] input[name="csrf"]');
                        if (!token) return false;
                        const controller = new AbortController();
                        const timer = setTimeout(() => controller.abort(), 5000);
                        try {
                            const r = await fetch('/logout', {method:'POST', signal:controller.signal,
                                body:new URLSearchParams({csrf:token.value})});
                            return r.ok;
                        } finally { clearTimeout(timer); }
                    }""")
                except Exception:
                    report['logged_out'] = False
                context.close()
                browser.close()
    except Exception as error:
        report['errors'].append({'stage': current, 'error': error_text(error)})
    finally:
        save_report()
        print(f'\nResults: {output}\nReport: {report_path}', flush=True)
        print(f'{len(report["pages"])} screenshots; {len(report["errors"])} errors.')
    return 1 if report['errors'] else 0


if __name__ == '__main__':
    sys.exit(main())
