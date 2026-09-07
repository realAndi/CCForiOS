"""Full-scope Claude Code sign-in for iOS, and keeping it signed in.

Background
----------
Claude Code stores credentials through Bun.secrets, which cannot write on iOS.
Two consequences, and this module addresses both:

  * `claude auth login` completes and then silently discards the result.
  * `claude setup-token` works, but its token is deliberately limited to
    user:inference, which costs Remote Control, MCP servers and file upload.

But Claude Code *reads* ~/.claude/.credentials.json perfectly well -- that is
how a credential copied from a desktop works, right up until its access token
expires ~8 hours later and cannot be refreshed in place.

So: do the OAuth flow ourselves for the full scope set, write that file, and
refresh it before each run. Everything Claude Code needs, nothing it has to
write.

Refresh-token rotation
----------------------
The token endpoint issues a *new* refresh token on every refresh and invalidates
the old one. Losing the replacement logs you out, so the write path here is
paranoid: an exclusive lock so two concurrent starts cannot both refresh, an
atomic replace so a crash cannot truncate the file, and no destructive step
anywhere on failure -- a refresh that fails leaves the existing credential
exactly as it was.
"""

import base64
import errno
import fcntl
import hashlib
import json
import os
import secrets
import time
import urllib.error
import urllib.parse
import urllib.request

CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
AUTHORIZE = "https://claude.com/cai/oauth/authorize"
TOKEN = "https://platform.claude.com/v1/oauth/token"
REDIRECT = "https://platform.claude.com/oauth/code/callback"

# Exactly what `claude auth login` requests. Anything less and features quietly
# switch off -- user:profile alone is the difference between Remote Control
# working and not.
SCOPES = [
    "org:create_api_key",
    "user:profile",
    "user:inference",
    "user:sessions:claude_code",
    "user:mcp_servers",
    "user:file_upload",
]

HOME = os.path.expanduser("~")

# Our master copy. Claude Code knows nothing about this path, which is the whole
# point: it treats ~/.claude/.credentials.json as a *legacy* file, and once it
# has read a valid credential from there it migrates it into Bun.secrets and
# deletes the original. On iOS the write half of that silently fails and the
# delete half succeeds, so a credential written straight to .credentials.json
# survives exactly one run. (An invalid one is left alone -- the migration never
# gets that far -- which is why the effect looks intermittent.)
#
# So we own the master, and re-materialise .credentials.json before every
# launch. Claude Code may delete its copy as often as it likes.
STORE = os.path.join(HOME, ".claude", "ccauth.json")
CRED = os.path.join(HOME, ".claude", ".credentials.json")
LOCK = os.path.join(HOME, ".claude", "ccauth.lock")

# Cloudflare rejects a request that does not look like the CLI (403, code 1010).
HEADERS = {
    "content-type": "application/json",
    "accept": "application/json",
    "user-agent": "claude-cli/2.1.263 (external, cli)",
    "anthropic-beta": "oauth-2025-04-20",
}

REFRESH_MARGIN = 30 * 60      # refresh this long before expiry
NET_TIMEOUT = 45


class AuthError(Exception):
    pass


def _post(payload):
    req = urllib.request.Request(TOKEN, data=json.dumps(payload).encode(), headers=HEADERS)
    try:
        with urllib.request.urlopen(req, timeout=NET_TIMEOUT) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        detail = e.read()[:300].decode("utf-8", "replace").strip()
        raise AuthError("token endpoint returned HTTP %s: %s" % (e.code, detail))
    except Exception as e:
        raise AuthError("could not reach the token endpoint: %s" % e)


def _to_credentials(resp, previous=None):
    """Shape a token response into the file Claude Code reads.

    Unknown fields from an existing credential are preserved: the file is
    Anthropic's format, not ours, and dropping a field we do not recognise
    would be a silent downgrade.
    """
    now = int(time.time() * 1000)
    oauth = dict((previous or {}).get("claudeAiOauth", {}))
    oauth["accessToken"] = resp["access_token"]
    if resp.get("refresh_token"):
        oauth["refreshToken"] = resp["refresh_token"]
    if resp.get("expires_in"):
        oauth["expiresAt"] = now + int(resp["expires_in"]) * 1000
    if resp.get("refresh_token_expires_in"):
        oauth["refreshTokenExpiresAt"] = now + int(resp["refresh_token_expires_in"]) * 1000
    if resp.get("scope"):
        oauth["scopes"] = resp["scope"].split()
    # A refresh response carries subscription_type only sometimes; the
    # authorization_code exchange does not carry it at all. Keep whatever we
    # already had rather than overwriting a real value with nothing.
    acct = resp.get("account") or {}
    org = resp.get("organization") or {}
    sub = acct.get("subscription_type") or org.get("billing_type")
    if sub:
        oauth["subscriptionType"] = sub

    out = dict(previous or {})
    out["claudeAiOauth"] = oauth

    # Who this credential belongs to, for `claude-login --status`. Kept under
    # our own key and stripped before publishing, so the file Claude Code reads
    # stays exactly the shape it expects.
    meta = dict(out.get("ccauth") or {})
    if acct.get("email_address"):
        meta["email"] = acct["email_address"]
    if org.get("name"):
        meta["organization"] = org["name"]
    if meta:
        out["ccauth"] = meta
    return out


def _write_atomic(path, creds):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(creds, f)
        f.flush()
        os.fsync(f.fileno())          # the new refresh token must survive a crash
    os.replace(tmp, path)
    os.chmod(path, 0o600)


def _write(creds):
    """Persist to the master, then publish a copy for Claude Code to read."""
    _write_atomic(STORE, creds)
    materialise(creds)


def materialise(creds=None):
    """(Re-)create .credentials.json from the master.

    Called before every launch by the wrapper, because Claude Code deletes its
    copy after migrating it. Cheap, and it makes the deletion a non-event.
    """
    creds = creds or _read_file(STORE)
    if not creds:
        return False
    published = {k: v for k, v in creds.items() if k != "ccauth"}
    try:
        if _read_file(CRED) == published:
            return False                      # already current, leave it alone
        _write_atomic(CRED, published)
        return True
    except OSError:
        return False


def _read_file(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def read():
    """The master, or a credential written before the master existed."""
    return _read_file(STORE) or _read_file(CRED)


def _lock():
    os.makedirs(os.path.dirname(LOCK), exist_ok=True)
    fd = os.open(LOCK, os.O_WRONLY | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
    except OSError:
        os.close(fd)
        raise
    return fd


def refresh_if_needed(force=False):
    """Refresh the access token when it is close to expiry.

    Returns (changed, message). Never raises: a failure here must not stop
    Claude Code from starting -- the existing token may still be good, and an
    expired one produces a clearer error from the app than from us.
    """
    creds = read()
    if not creds:
        return False, None
    oauth = creds.get("claudeAiOauth") or {}
    refresh_token = oauth.get("refreshToken")
    if not refresh_token:
        return False, None

    expires_at = oauth.get("expiresAt") or 0
    if not force and expires_at > (time.time() + REFRESH_MARGIN) * 1000:
        return False, None

    try:
        fd = _lock()
    except OSError as e:
        return False, "could not lock credentials: %s" % e
    try:
        # Another process may have refreshed while we waited for the lock.
        creds = read() or creds
        oauth = creds.get("claudeAiOauth") or {}
        refresh_token = oauth.get("refreshToken", refresh_token)
        expires_at = oauth.get("expiresAt") or 0
        if not force and expires_at > (time.time() + REFRESH_MARGIN) * 1000:
            return False, None

        resp = _post({
            "grant_type": "refresh_token",
            "refresh_token": refresh_token,
            "client_id": CLIENT_ID,
        })
        _write(_to_credentials(resp, creds))
        return True, None
    except AuthError as e:
        return False, str(e)
    except Exception as e:
        return False, "unexpected error refreshing: %s" % e
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)


def authorize_url():
    """Build the sign-in URL, returning it with the PKCE verifier and state."""
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip("=")
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
    state = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip("=")
    query = urllib.parse.urlencode({
        "code": "true",
        "client_id": CLIENT_ID,
        "response_type": "code",
        "redirect_uri": REDIRECT,
        "scope": " ".join(SCOPES),
        "code_challenge": challenge,
        "code_challenge_method": "S256",
        "state": state,
    })
    return "%s?%s" % (AUTHORIZE, query), verifier, state


def exchange(code, verifier, state):
    """Trade the pasted code for tokens and write them.

    The callback page shows `code#state`; accept either that or a bare code,
    since people paste both.
    """
    code = code.strip()
    if "#" in code:
        code, _, pasted_state = code.partition("#")
        code, pasted_state = code.strip(), pasted_state.strip()
        if pasted_state and pasted_state != state:
            raise AuthError("state mismatch -- the pasted code is from a "
                            "different sign-in attempt; start again")
    if not code:
        raise AuthError("no code given")

    resp = _post({
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT,
        "client_id": CLIENT_ID,
        "code_verifier": verifier,
        "state": state,
    })
    if not resp.get("access_token"):
        raise AuthError("no access token in the response")
    creds = _to_credentials(resp, read())
    _write(creds)
    return creds["claudeAiOauth"].get("scopes", [])
