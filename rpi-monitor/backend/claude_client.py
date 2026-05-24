"""
Claude.ai client: Playwright-based login + lightweight httpx polling.

First run:  login_playwright(email, password)  → saves cookies to cookies.json
Subsequent: poll_limits()                       → uses saved cookies via httpx
Fallback:   poll_with_playwright()              → loads full page if httpx fails
"""

import asyncio
import json
import os
import time
from pathlib import Path
from typing import Optional, Callable

import httpx

try:
    from playwright.async_api import async_playwright
    HAS_PLAYWRIGHT = True
except ImportError:
    HAS_PLAYWRIGHT = False

COOKIES_FILE = Path(__file__).parent / 'cookies.json'
ENDPOINTS_FILE = Path(__file__).parent / 'endpoints.json'
CHROMIUM_PATH = os.getenv('CHROMIUM_PATH', '')  # empty = use Playwright's bundled browser

# Candidate endpoints to try when polling (discovered dynamically during login)
CANDIDATE_URLS = [
    'https://claude.ai/api/organizations',
    'https://claude.ai/api/account',
    'https://claude.ai/api/me',
    'https://claude.ai/api/bootstrap',
]

_BROWSER_HEADERS = {
    'User-Agent': (
        'Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 '
        '(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36'
    ),
    'Accept': 'application/json, text/plain, */*',
    'Accept-Language': 'it-IT,it;q=0.9,en;q=0.8',
    'Referer': 'https://claude.ai/',
    'Sec-Fetch-Dest': 'empty',
    'Sec-Fetch-Mode': 'cors',
    'Sec-Fetch-Site': 'same-origin',
}


# ── Limit data extractor (same logic as Tampermonkey userscript) ──────────────

def extract_account_limits(obj, depth: int = 0) -> Optional[dict]:
    """Recursively search any JSON object for claude.ai account limit fields."""
    if not obj or not isinstance(obj, (dict, list)) or depth > 8:
        return None

    if isinstance(obj, list):
        for item in obj:
            r = extract_account_limits(item, depth + 1)
            if r:
                return r
        return None

    keys_str = ' '.join(str(k) for k in obj.keys()).lower()

    looks_like_limit = (
        'remaining' in keys_str or
        'messages_left' in keys_str or
        ('limit' in keys_str and ('used' in keys_str or 'reset' in keys_str)) or
        'rate_limit' in keys_str or
        'quota' in keys_str or
        ('usage' in keys_str and 'reset' in keys_str)
    )

    if looks_like_limit:
        r: dict = {}
        for k, v in obj.items():
            kl = str(k).lower()
            if ('remaining' in kl or 'left' in kl) and v is not None:
                try:
                    r['messages_remaining'] = int(v)
                except (TypeError, ValueError):
                    pass
            if ('limit' in kl or 'max' in kl or 'total' in kl) and isinstance(v, (int, float)) and v > 0:
                r['messages_limit'] = int(v)
            if ('used' in kl or 'consumed' in kl or 'count' in kl) and isinstance(v, (int, float)):
                r['messages_used'] = int(v)
            if 'reset' in kl and v:
                r['reset_at'] = v
            if ('plan' in kl or 'tier' in kl) and v:
                r['plan'] = str(v)

        if 'messages_remaining' in r or 'messages_limit' in r:
            if 'messages_used' not in r and 'messages_limit' in r and 'messages_remaining' in r:
                r['messages_used'] = r['messages_limit'] - r['messages_remaining']
            return r

    for v in obj.values():
        if isinstance(v, (dict, list)):
            found = extract_account_limits(v, depth + 1)
            if found:
                return found

    return None


# ── ClaudeClient ──────────────────────────────────────────────────────────────

class ClaudeClient:
    def __init__(self, on_limits_found: Optional[Callable[[dict], None]] = None):
        self.on_limits_found = on_limits_found
        self._discovered_url: Optional[str] = None
        self._load_endpoints()

    def _load_endpoints(self):
        if ENDPOINTS_FILE.exists():
            try:
                self._discovered_url = json.loads(ENDPOINTS_FILE.read_text()).get('limits_url')
            except Exception:
                pass

    def _save_endpoints(self, url: str):
        self._discovered_url = url
        ENDPOINTS_FILE.write_text(json.dumps({'limits_url': url}))

    def has_cookies(self) -> bool:
        return COOKIES_FILE.exists()

    def get_cookie_age_seconds(self) -> Optional[float]:
        if not COOKIES_FILE.exists():
            return None
        return time.time() - COOKIES_FILE.stat().st_mtime

    def _httpx_cookies(self) -> dict:
        if not COOKIES_FILE.exists():
            return {}
        try:
            raw = json.loads(COOKIES_FILE.read_text())
            return {c['name']: c['value'] for c in raw}
        except Exception:
            return {}

    # ── Playwright login ──────────────────────────────────────────────────────

    async def login_playwright(self, email: str, password: str) -> tuple[bool, str]:
        """Log in to claude.ai using a headless browser. Saves cookies on success."""
        if not HAS_PLAYWRIGHT:
            return False, 'Playwright non è installato. Esegui: pip install playwright && playwright install chromium'

        try:
            async with async_playwright() as pw:
                launch_opts: dict = {'headless': True}
                if CHROMIUM_PATH:
                    launch_opts['executable_path'] = CHROMIUM_PATH

                browser = await pw.chromium.launch(**launch_opts)
                ctx = await browser.new_context(
                    user_agent=_BROWSER_HEADERS['User-Agent'],
                    viewport={'width': 1280, 'height': 800},
                )
                page = await ctx.new_page()

                found_limits: dict = {}
                found_url: list = [None]

                async def handle_response(response):
                    try:
                        if 'claude.ai' not in response.url or response.status != 200:
                            return
                        ct = response.headers.get('content-type', '')
                        if 'application/json' not in ct:
                            return
                        body = await response.json()
                        limits = extract_account_limits(body)
                        if limits:
                            found_limits.update(limits)
                            if found_url[0] is None:
                                found_url[0] = response.url
                    except Exception:
                        pass

                page.on('response', handle_response)

                # Navigate to login page
                await page.goto('https://claude.ai/login', wait_until='domcontentloaded', timeout=30_000)
                await page.wait_for_timeout(1500)

                # Email step
                try:
                    email_sel = 'input[type="email"], input[name="email"], input[autocomplete="email"]'
                    await page.locator(email_sel).first.fill(email)
                    await page.keyboard.press('Tab')
                    await page.wait_for_timeout(300)
                    # Some login pages show password after clicking Continue
                    submit_btn = page.locator('button[type="submit"]').first
                    if await submit_btn.is_visible():
                        await submit_btn.click()
                    await page.wait_for_timeout(1500)
                except Exception as e:
                    print(f'[login] email step: {e}')

                # Password step
                try:
                    await page.locator('input[type="password"]').first.fill(password)
                    await page.keyboard.press('Enter')
                except Exception as e:
                    print(f'[login] password step: {e}')

                # Wait for successful navigation
                try:
                    await page.wait_for_url('https://claude.ai/**', timeout=30_000)
                    await page.wait_for_load_state('networkidle', timeout=15_000)
                except Exception:
                    pass

                await page.wait_for_timeout(3000)

                cookies = await ctx.cookies()
                await browser.close()

                if not cookies:
                    return False, 'Accesso fallito: nessun cookie ottenuto. Controlla email e password.'

                COOKIES_FILE.write_text(json.dumps(cookies))
                print(f'[login] salvati {len(cookies)} cookie')

                if found_url[0]:
                    self._save_endpoints(found_url[0])
                    print(f'[login] endpoint limiti: {found_url[0]}')

                return True, f'Accesso riuscito ({len(cookies)} cookie)'

        except Exception as e:
            return False, f'Errore accesso: {str(e)}'

    # ── httpx poll (fast, lightweight) ───────────────────────────────────────

    async def poll_limits(self) -> Optional[dict]:
        """Try to get account limits via httpx with saved cookies. Fast path."""
        cookies = self._httpx_cookies()
        if not cookies:
            return None

        urls = []
        if self._discovered_url:
            urls.append(self._discovered_url)
        urls.extend(u for u in CANDIDATE_URLS if u != self._discovered_url)

        async with httpx.AsyncClient(
            headers=_BROWSER_HEADERS,
            cookies=cookies,
            follow_redirects=True,
            timeout=15.0,
        ) as client:
            for url in urls:
                try:
                    resp = await client.get(url)
                    if resp.status_code in (401, 403):
                        print('[poll] sessione scaduta')
                        return {'_error': 'session_expired'}
                    if resp.status_code != 200:
                        continue
                    try:
                        data = resp.json()
                    except Exception:
                        continue
                    limits = extract_account_limits(data)
                    if limits:
                        if url != self._discovered_url:
                            self._save_endpoints(url)
                        print(f'[poll] limiti trovati via httpx: {url}')
                        return limits
                except Exception as e:
                    print(f'[poll] errore {url}: {e}')

        return None

    # ── Playwright fallback poll (slow, reliable) ─────────────────────────────

    async def poll_with_playwright(self) -> Optional[dict]:
        """Full page load to extract limits when httpx fails. Slower (~20s)."""
        if not HAS_PLAYWRIGHT or not COOKIES_FILE.exists():
            return None

        try:
            async with async_playwright() as pw:
                launch_opts: dict = {'headless': True}
                if CHROMIUM_PATH:
                    launch_opts['executable_path'] = CHROMIUM_PATH

                browser = await pw.chromium.launch(**launch_opts)
                ctx = await browser.new_context(user_agent=_BROWSER_HEADERS['User-Agent'])

                saved_cookies = json.loads(COOKIES_FILE.read_text())
                await ctx.add_cookies(saved_cookies)

                page = await ctx.new_page()
                found_limits: list = [None]
                found_url: list = [None]

                async def handle_response(response):
                    if found_limits[0]:
                        return
                    try:
                        if 'claude.ai' not in response.url or response.status != 200:
                            return
                        ct = response.headers.get('content-type', '')
                        if 'application/json' not in ct:
                            return
                        body = await response.json()
                        limits = extract_account_limits(body)
                        if limits:
                            found_limits[0] = limits
                            found_url[0] = response.url
                    except Exception:
                        pass

                page.on('response', handle_response)

                try:
                    await page.goto('https://claude.ai/', wait_until='networkidle', timeout=30_000)
                    await page.wait_for_timeout(3000)
                except Exception:
                    pass

                if found_url[0]:
                    self._save_endpoints(found_url[0])

                # Refresh cookies
                new_cookies = await ctx.cookies()
                if new_cookies:
                    COOKIES_FILE.write_text(json.dumps(new_cookies))

                await browser.close()
                return found_limits[0]

        except Exception as e:
            print(f'[pw-poll] errore: {e}')
            return None
