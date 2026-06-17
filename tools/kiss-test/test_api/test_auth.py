"""POST /api/auth/login, POST /api/auth/logout."""
import requests


def test_login_success_returns_token(devices):
    d = devices[0]
    r = requests.post(f"{d.url}/api/auth/login",
                      json={"identity_id": d.identity, "password": d.password},
                      timeout=10)
    assert r.status_code == 200, r.text
    body = r.json()
    assert isinstance(body["token"], str) and len(body["token"]) >= 16
    assert body["identity_id"] == d.identity
    assert isinstance(body["expires_in_s"], int) and body["expires_in_s"] > 0


def test_login_wrong_password_401(devices):
    d = devices[0]
    r = requests.post(f"{d.url}/api/auth/login",
                      json={"identity_id": d.identity, "password": "nope"},
                      timeout=10)
    assert r.status_code == 401
    body = r.json()
    assert "error" in body
    # Per `feedback_descriptive_error_messages`: every error response
    # must carry a human-readable `message` alongside the slug.
    assert "message" in body and len(body["message"]) > 0


def test_login_missing_identity_400(devices):
    d = devices[0]
    r = requests.post(f"{d.url}/api/auth/login",
                      json={"password": d.password},
                      timeout=10)
    assert r.status_code in (400, 401)


def test_protected_endpoint_without_token_401(devices):
    d = devices[0]
    r = requests.get(f"{d.url}/api/state", timeout=5)
    assert r.status_code == 401


def test_protected_endpoint_with_bad_token_401(devices):
    d = devices[0]
    r = requests.get(
        f"{d.url}/api/state",
        headers={"Authorization": "Bearer not_a_real_token_xx"},
        timeout=5,
    )
    assert r.status_code == 401
