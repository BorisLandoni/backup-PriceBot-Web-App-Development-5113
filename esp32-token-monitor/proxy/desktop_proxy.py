"""
Proxy MITM per Claude Desktop (e qualsiasi altra app)
Intercetta le risposte HTTPS di claude.ai e api.anthropic.com,
estrae i token e li invia al server ESP32.

Requisiti:
    pip install mitmproxy requests

Avvio (una volta installato mitmproxy):
    mitmdump -p 8080 -s desktop_proxy.py

Poi configura il proxy di sistema:
    Mac:     Preferenze di Sistema → Rete → Avanzate → Proxy
             HTTP  → 127.0.0.1 : 8080
             HTTPS → 127.0.0.1 : 8080
    Windows: Impostazioni → Rete → Proxy manuale
             127.0.0.1 : 8080

Primo avvio — trust del certificato (UNA VOLTA):
    Mac:     sudo security add-trusted-cert -d -r trustRoot \\
             -k /Library/Keychains/System.keychain ~/.mitmproxy/mitmproxy-ca-cert.pem
    Windows: mitmproxy genera il cert in %USERPROFILE%\\.mitmproxy\\
             Doppio clic su mitmproxy-ca-cert.p12 → Importa in "Autorità radice attendibili"

Dopo il trust, Claude Desktop e qualsiasi browser/app usa il proxy
e i token vengono tracciati automaticamente.
"""

import json
import requests
from mitmproxy import http

TOKEN_SERVER = "http://127.0.0.1:3333/api/tokens"

# Domini da monitorare
CLAUDE_HOSTS = {
    "api.anthropic.com",
    "claude.ai",
}

# Path che contengono risposte con token
COMPLETION_PATHS = ("/completion", "/messages", "/v1/messages")


class ClaudeTokenMonitor:

    def _is_claude_api(self, flow: http.HTTPFlow) -> bool:
        host = flow.request.pretty_host
        path = flow.request.path
        return (
            any(h in host for h in CLAUDE_HOSTS)
            and any(p in path for p in COMPLETION_PATHS)
        )

    def _extract_tokens(self, body: str) -> dict:
        inp = out = cache_read = cache_creation = 0

        # Prova JSON diretto (non-streaming)
        try:
            j = json.loads(body)
            if "usage" in j:
                u = j["usage"]
                inp           = u.get("input_tokens", 0)
                out           = u.get("output_tokens", 0)
                cache_read    = u.get("cache_read_input_tokens", 0)
                cache_creation= u.get("cache_creation_input_tokens", 0)
            return {"input": inp, "output": out, "cache_read": cache_read, "cache_creation": cache_creation}
        except json.JSONDecodeError:
            pass

        # Parsing SSE (streaming)
        for line in body.splitlines():
            if not line.startswith("data: "):
                continue
            raw = line[6:].strip()
            if not raw or raw == "[DONE]":
                continue
            try:
                evt = json.loads(raw)
                if evt.get("type") == "message_start":
                    u = evt.get("message", {}).get("usage", {})
                    inp           = u.get("input_tokens", 0)
                    cache_read    = u.get("cache_read_input_tokens", 0)
                    cache_creation= u.get("cache_creation_input_tokens", 0)
                elif evt.get("type") == "message_delta":
                    out = evt.get("usage", {}).get("output_tokens", 0)
            except json.JSONDecodeError:
                continue

        return {"input": inp, "output": out, "cache_read": cache_read, "cache_creation": cache_creation}

    def response(self, flow: http.HTTPFlow) -> None:
        if not self._is_claude_api(flow):
            return

        body = flow.response.content.decode("utf-8", errors="ignore")
        t = self._extract_tokens(body)

        if t["input"] + t["output"] == 0:
            return

        try:
            requests.post(TOKEN_SERVER, json={
                "input_tokens":          t["input"],
                "output_tokens":         t["output"],
                "cache_read_tokens":     t["cache_read"],
                "cache_creation_tokens": t["cache_creation"],
            }, timeout=2)
            print(f"[Claude Desktop] in:{t['input']}  out:{t['output']}  cache:{t['cache_read']}")
        except requests.RequestException:
            pass  # server.js offline, ignora silenziosamente


addons = [ClaudeTokenMonitor()]
