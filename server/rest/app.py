import base64
import codecs
import http.cookies
import json
import logging
import typing
import urllib.parse

import badbadbad  # type: ignore
import pysm  # type: ignore

import api
import api_config as config  # type: ignore

log = logging.getLogger("api")
log.setLevel(logging.DEBUG)
fh = logging.FileHandler(config.logfile)
fh.setLevel(logging.DEBUG)
formatter = logging.Formatter(
    "%(asctime)s|%(name)s|%(levelname)s|%(message)s", "%s"
)
fh.setFormatter(formatter)
log.addHandler(fh)

_pysm_login_set = False


def set_pysm_logging() -> None:
    global _pysm_login_set
    if not _pysm_login_set:
        _pysm_login_set = True

        # The global state seems confusing.  You'd think that you could
        # just call this during module import but that doesn't seem to work.
        # Perhaps there's some forking happens where the pysm module is getting
        # reloaded and the global list of logging paths is getting dropped?
        pysm.log_to_file(config.logfile, "debug")


JsonObj = typing.Dict[str, typing.Any]


Chunks = typing.List[typing.Union[str, bytes]]


class HTTP:
    """An HTTP-like response"""

    def __init__(
        self,
        firstline: str,
        content_type: str = "text/plain",
        chunks: typing.Optional[Chunks] = None,
    ) -> None:
        self.firstline = firstline
        self.content_type = content_type
        self.chunks = chunks or []


Response = typing.Union[api.API, HTTP]


class AuthData:
    def __init__(
        self,
        path: str,
        token: typing.Optional[str],
        sig: typing.Optional[str],
        basic_auth: typing.Optional[str],
        session: typing.Optional[str],
        payload: bytes,
        body: JsonObj,
    ):
        # from wsgi
        self.path = path
        # from headers
        self.token = token
        self.sig = sig
        self.basic_auth = basic_auth
        # from cookies
        self.session = session
        # from request body
        self.payload = payload
        self.body = body


class AuthStatus:
    def __init__(self) -> None:
        self.decided = False
        self.ok = False
        self.why = "authentication required"


class BadAuth(AuthStatus):
    def __init__(self, why: str) -> None:
        self.decided = True
        self.ok = False
        self.why = why


class GoodAuth(AuthStatus):
    def __init__(self, uuid: bytes) -> None:
        self.decided = True
        self.ok = True
        self.why = ""
        self.uuid = uuid


def check_basic_auth(
    status: AuthStatus, auth: AuthData, smsql: typing.Any
) -> AuthStatus:
    if status.decided or auth.basic_auth is None:
        return status
    if not auth.basic_auth.startswith("Basic "):
        return BadAuth("bad AUTHENTICATION header")
    dec_ah = base64.b64decode(auth.basic_auth[6:])
    split = dec_ah.find(b":")
    if split == -1:
        return BadAuth("bad AUTHENTICATION header")
    email = dec_ah[:split]
    password = dec_ah[split + 1 :]
    # get the uuid
    try:
        uuid = smsql.validate_login(email, password)
    except pysm.UserError as e:
        return BadAuth(str(e))
    return GoodAuth(uuid)


ValidateFn = typing.Callable[[int, int, bytes, bytes], bytes]


def token_auth(
    status: AuthStatus, auth: AuthData, validate_fn: ValidateFn
) -> AuthStatus:
    if status.decided or auth.token is None:
        return status

    if auth.sig is None:
        return BadAuth("signature not provided in header of POST request")
    try:
        sig = codecs.decode(auth.sig, "hex")
    except:
        return BadAuth("signature is not properly hex-encoded")

    if len(auth.payload) == 0:
        return BadAuth("payload not provided in body of POST request")

    try:
        token = int(auth.token)
    except:
        return BadAuth("invalid token")
    nonce = auth.body.get("nonce")
    if nonce is None:
        return BadAuth("nonce not provided in payload")
    try:
        nonce = int(nonce)
        if nonce < 0:
            raise ValueError()
    except:
        return BadAuth("invalid nonce in payload")

    # check body/path agreement
    if auth.body.get("path") != auth.path:
        return BadAuth("path of POST request does not match path in payload")

    try:
        uuid = validate_fn(token, nonce, auth.payload, sig)
    except pysm.UserError as e:
        return BadAuth(str(e))

    return GoodAuth(uuid)


def check_token_auth(
    status: AuthStatus, auth: AuthData, smsql: typing.Any
) -> AuthStatus:
    return token_auth(status, auth, smsql.validate_token_auth)


def check_installation_auth(
    status: AuthStatus, auth: AuthData, smsql: typing.Any
) -> AuthStatus:
    return token_auth(status, auth, smsql.validate_installation_auth)


def check_session_auth(
    status: AuthStatus, auth: AuthData, smsql: typing.Any
) -> AuthStatus:
    if status.decided or auth.session is None:
        return status

    server_id = config.server_id

    csrf = auth.body.get("csrf_token")
    if csrf is None:
        return BadAuth("403 missing csrf token")

    try:
        # this will also update the last_seen time
        uuid = smsql.validate_session_auth(server_id, auth.session)
        smsql.validate_csrf(auth.session, csrf)
    except pysm.UserError as e:
        return BadAuth(str(e))

    return GoodAuth(uuid)


# AuthFn decorates an ApiFn, returning a AuthApiFn.

ApiFn = typing.Callable[[bytes, typing.Optional[str], typing.Any], Response]

AuthApiFn = typing.Callable[
    [AuthData, typing.Optional[str], typing.Any], Response
]

AuthFn = typing.Callable[[ApiFn], AuthApiFn]


def apicall(
    fn: ApiFn, uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> Response:
    """Catch pysm.UserErrors thrown by the api function."""
    try:
        return fn(uuid, arg, smsql)
    except pysm.UserError as e:
        return api.API.error(str(e))


def password_auth(fn: ApiFn) -> AuthApiFn:
    """Password required for authentication."""

    def _fn(
        auth: AuthData, arg: typing.Optional[str], smsql: typing.Any
    ) -> Response:
        if auth.basic_auth is None:
            return HTTP("403 HTTP Basic Auth required for this endpoint")
        status = AuthStatus()
        status = check_basic_auth(status, auth, smsql)
        if not isinstance(status, GoodAuth):
            return HTTP("403 " + status.why)
        return apicall(fn, status.uuid, arg, smsql)

    return _fn


def regular_auth(fn: ApiFn) -> AuthApiFn:
    """Normal authentication: password, user token, or session."""

    def _fn(
        auth: AuthData, arg: typing.Optional[str], smsql: typing.Any
    ) -> Response:
        # do the appropriate authentication check
        status = AuthStatus()
        status = check_basic_auth(status, auth, smsql)
        status = check_session_auth(status, auth, smsql)
        status = check_token_auth(status, auth, smsql)
        if not isinstance(status, GoodAuth):
            return HTTP("403 " + status.why)
        return apicall(fn, status.uuid, arg, smsql)

    return _fn


def installation_auth(fn: ApiFn) -> AuthApiFn:
    """Installation authentiation: uses installation tokens."""

    def _fn(
        auth: AuthData, arg: typing.Optional[str], smsql: typing.Any
    ) -> Response:
        status = AuthStatus()
        if auth.token is None:
            return HTTP("403 installation token required for this endpoint")
        status = AuthStatus()
        status = check_installation_auth(status, auth, smsql)
        if not isinstance(status, GoodAuth):
            return HTTP("403 " + status.why)
        return apicall(fn, status.uuid, arg, smsql)

    return _fn


ROUTES: typing.Dict[str, AuthApiFn] = {
    "/api/list_aliases": regular_auth(api.list_aliases),
    "/api/add_random_alias": regular_auth(api.add_random_alias),
    "/api/add_primary_alias": regular_auth(api.add_primary_alias),
    "/api/delete_alias": regular_auth(api.delete_alias),
    "/api/list_devices": regular_auth(api.list_devices),
    "/api/add_device": password_auth(api.add_device),
    "/api/delete_device": regular_auth(api.delete_device),
    "/api/list_tokens": regular_auth(api.list_tokens),
    "/api/add_token": password_auth(api.add_token),
    "/api/delete_token": regular_auth(api.delete_token),
    "/api/add_installation": password_auth(api.add_installation),
    "/api/list_installations": regular_auth(api.list_installations),
    "/api/delete_installation": regular_auth(api.delete_installation),
    "/api/delete_installation_by_token": installation_auth(
        api.delete_installation_by_token
    ),
    "/api/set_challenge": installation_auth(api.set_challenge),
    "/api/delete_challenge": installation_auth(api.delete_challenge),
    "/api/account_info": regular_auth(api.account_info),
    # disabled for now due to me not loving the user interface
    #   '/api/delete_all_aliases'  : password_auth(api.delete_all_aliases),
    # disabled for now due to permissions issues
    #   '/api/delete_all_mail'     : password_auth(api.delete_all_mail),
    "/api/change_password": password_auth(api.change_password),
}


def load_body(payload: bytes) -> typing.Tuple[JsonObj, bool]:
    # allow non-token-auth requests to use non-base64-encoded body
    # that simplifies the browser-side javascript code a lot
    # because utf8 and base64 is pure insanity:
    # https://developer.mozilla.org/en-US/docs/Web/API/WindowBase64/Base64_encoding_and_decoding
    # Note: I still have a btoa() call to do the Basic Auth header
    if len(payload) == 0:
        body = {}  # type: JsonObj
        return body, True
    try:
        # try to read it as json first
        body = json.loads(payload.decode("utf8"))
        assert isinstance(body, dict)
        assert all(isinstance(k, str) for k in body)
        return body, True
    except:
        pass
    try:
        # if we can't read it as json, try to b64decode it first
        body = json.loads(base64.b64decode(payload).decode("utf8"))
        assert isinstance(body, dict)
        assert all(isinstance(k, str) for k in body)
        return body, True
    except:
        return {}, False


def app(environ: typing.Dict[str, typing.Any]) -> Response:
    set_pysm_logging()
    log = logging.getLogger("api.app")
    method = environ["REQUEST_METHOD"]
    # only allow POST methods
    if method != "POST":
        return HTTP("405 all api endpoints are POST only")

    try:
        # apache wsgi
        path = environ["REQUEST_URI"]
    except:
        # gunicorn
        path = environ["RAW_URI"]

    # parse cookies from HTTP request
    cookiestr = environ.get("HTTP_COOKIE", "")
    cookies = http.cookies.SimpleCookie(cookiestr)  # type: typing.Any
    session = None
    if "SPLINTER_SESSION" in cookies:
        session = urllib.parse.unquote(cookies["SPLINTER_SESSION"].value)

    log.info('app called with path "' + path + '"')

    # read body of request
    payload = environ["wsgi.input"].read(16384)
    try:
        payload.decode("utf8")
    except:
        return HTTP("422 payload is not valid utf8")

    body, ok = load_body(payload)
    if not ok:
        return HTTP(
            "422 POST body is not UTF8 json or base64 encoded UTF8 json"
        )

    auth = AuthData(
        path=path,
        token=environ.get("HTTP_X_AUTH_TOKEN"),
        sig=environ.get("HTTP_X_AUTH_SIGNATURE"),
        basic_auth=environ.get("HTTP_AUTHORIZATION"),
        session=session,
        payload=payload,
        body=body,
    )

    func = ROUTES.get(path)
    if func is None:
        return HTTP("404 api endpoint not found")

    arg = body.get("arg")

    with pysm.SMSQL(sock=config.sqlsock) as smsql:
        return func(auth, arg, smsql)


Headers = typing.List[typing.Tuple[str, str]]


def app_wrapper(
    environ: typing.Dict[str, typing.Any],
    start_fn: typing.Callable[[str, Headers], None],
) -> Chunks:
    try:
        resp = app(environ)
        if isinstance(resp, api.API):
            # insert suggested splintermail app version
            resp.json["minimum-client-version"] = 0.2
            start_fn("200 OK", [("Content-Type", "application/json")])
            return [json.dumps(resp.json).encode("utf8")]
        elif isinstance(resp, HTTP):
            headers = [("Content-Type", resp.content_type)]
            start_fn(resp.firstline, headers)
            return resp.chunks
        raise ValueError("unrecognized response type: {type(resp).__name__}")
    except:
        # send badbadbad alert
        badbadbad.alert_exc("exception in API code")
        # also log in api log file
        logging.exception("caught exception in app_wrapper")
        start_fn("500 internal server error", [("Content-Type", "text/plain")])
        return [b"we have been alerted, we will fix this"]
