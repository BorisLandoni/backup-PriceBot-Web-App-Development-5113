// Sends Claude API usage stats to the local token monitor server.
// Call reportTokenUsage() after every successful Claude API response.

const TOKEN_SERVER = import.meta.env.VITE_TOKEN_SERVER_URL || 'http://localhost:3333';

export async function reportTokenUsage({
  input_tokens = 0,
  output_tokens = 0,
  cache_read_tokens = 0,
  cache_creation_tokens = 0,
} = {}) {
  try {
    await fetch(`${TOKEN_SERVER}/api/tokens`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        input_tokens,
        output_tokens,
        cache_read_tokens,
        cache_creation_tokens,
      }),
    });
  } catch {
    // Non-critical — don't break the app if the monitor server is offline
  }
}

export async function resetTokenStats() {
  try {
    await fetch(`${TOKEN_SERVER}/api/tokens`, { method: 'DELETE' });
  } catch {
    // ignore
  }
}

export async function getTokenStats() {
  const res = await fetch(`${TOKEN_SERVER}/api/tokens`);
  if (!res.ok) throw new Error('Token server unreachable');
  return res.json();
}
