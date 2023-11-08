import codecs
import logging
import socket
import typing

import pysm  # type: ignore
import api_config as config  # type: ignore


class API:
    """An API-like repsonse"""

    def __init__(self, json: typing.Dict[str, typing.Any]):
        self.json = json

    @classmethod
    def success(cls, contents: typing.Any) -> "API":
        return cls({"status": "success", "contents": contents})

    @classmethod
    def error(cls, contents: str) -> "API":
        return cls({"status": "error", "contents": contents})


max_devices = 10
max_random_aliases = 1000


def list_aliases(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.list_aliases")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    aliases = smsql.list_aliases(uuid)
    random_aliases = [a for a, paid in aliases if not paid]
    primary_aliases = [a for a, paid in aliases if paid]
    return API.success(
        {
            "random_aliases": random_aliases,
            "primary_aliases": primary_aliases,
        },
    )


def add_random_alias(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    global max_random_aliases
    log = logging.getLogger("api.add_random_alias")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    alias = smsql.add_random_alias(uuid)
    return API.success(alias)


def add_primary_alias(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.add_primary_alias")
    log.info("called")
    return API.error("primary aliases disabled during beta testing")
    if arg is None:
        return API.error("command requires an argument")
    alias = arg
    pysm.valid_email(alias)
    pysm.add_primary_alias(uuid, alias)
    return API.success(alias)


def delete_alias(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.delete_alias")
    log.info("called")
    if arg is None:
        return API.error("command requires an argument")
    alias = arg
    smsql.delete_alias(uuid, alias)
    return API.success("ok")


def list_devices(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    global max_devices
    log = logging.getLogger("api.list_devices")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    fprs = smsql.list_device_fprs(uuid)
    return API.success(
        {
            "devices": fprs,
            "num_devices": len(fprs),
            "max_devices": max_devices,
        },
    )


def add_device(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    global max_devices
    log = logging.getLogger("api.add_device")
    log.info("called")
    if arg is None:
        return API.error("command requires an argument")
    public_key = arg
    fpr = smsql.add_device(uuid, public_key)
    num_devices = len(smsql.list_device_fprs(uuid))

    return API.success(
        {
            "device": fpr,
            "num_devices": num_devices,
            "max_devices": max_devices,
        },
    )


def delete_device(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    global max_devices
    log = logging.getLogger("api.delete_device")
    log.info("called")
    if arg is None:
        return API.error("command requires an argument")
    fpr = arg
    smsql.delete_device(uuid, fpr)
    return API.success(
        {
            "deleted": fpr,
        },
    )


def list_tokens(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.list_tokens")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    tokens = smsql.list_tokens(uuid)
    return API.success(
        {
            "tokens": tokens,
            "num_tokens": len(tokens),
        },
    )


def add_token(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.add_token")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    token, secret = smsql.add_token(uuid)
    return API.success(
        {
            "token": token,
            "secret": secret,
        },
    )


def delete_token(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.delete_token")
    log.info("called")
    if arg is None:
        return API.error("command requires an argument")
    try:
        token = int(arg)
        if token < 0:
            raise ValueError()
    except:
        return API.error("invalid token")
    smsql.delete_token(uuid, token)
    return API.success("ok")


def list_installations(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.list_installations")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    installations = smsql.list_installations(uuid)
    return API.success(
        {
            "installations": installations,
            "num_installations": len(installations),
        },
    )


def add_installation(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.add_installation")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    token, secret, subdomain, email = smsql.add_installation(uuid)
    return API.success(
        {
            "token": token,
            "secret": secret,
            "subdomain": subdomain,
            "email": email,
        },
    )


def delete_installation(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.delete_installation")
    log.info("called")
    if arg is None:
        return API.error("command requires an argument")
    subdomain = arg
    smsql.delete_installation(uuid, subdomain)
    return API.success("ok")


def delete_installation_by_token(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    # uuid is an installation uuid
    log = logging.getLogger("api.delete_installation_by_token")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    smsql.delete_installation_by_token(uuid)
    return API.success("ok")


def set_challenge(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    # uuid is an installation uuid
    log = logging.getLogger("api.set_challenge")
    log.info("called")
    if arg is None:
        return API.error("command requires an argument")
    challenge = arg
    smsql.set_challenge(uuid, challenge)
    hex_uuid = codecs.encode(uuid, "hex")

    with socket.socket(family=socket.AF_UNIX, type=socket.SOCK_STREAM) as sock:
        try:
            sock.connect(config.kvpsend_sock)
            sock.send(b"S:%s:%s\n" % (hex_uuid, challenge.encode("utf8")))
            resp = sock.recv(1)
            if not resp:
                log.error(f"api/dns got unexpected eof: {resp!r}")
            elif resp == b"k":
                # we expect an o"k" response
                return API.success({"result": "ok"})
            elif resp == b"t":
                # but we may see a "t"imeout response
                return API.success({"result": "timeout"})
            else:
                log.error(f"api/dns got unexpected response: {resp!r}")
        except Exception as e:
            log.error(f"api/dns: {e}")
        # any other response is an error
        return API.error("failed")


def delete_challenge(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    # uuid is an installation uuid
    log = logging.getLogger("api.delete_challenge")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    smsql.delete_challenge(uuid)
    return API.success("ok")


def account_info(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.account_info")
    log.info("called")
    if arg is not None:
        return API.error("command requires no arguments")
    num_devices, num_primary_aliases, num_random_aliases = smsql.account_info(
        uuid
    )
    return API.success(
        {
            "num_random_aliases": num_random_aliases,
            "num_primary_aliases": num_primary_aliases,
            "num_devices": num_devices,
        },
    )


def change_password(
    uuid: bytes, arg: typing.Optional[str], smsql: typing.Any
) -> API:
    log = logging.getLogger("api.change_password")
    log.info("called")
    if arg is None:
        return API.error("command requires an argument")
    new_password = arg

    smsql.change_password(uuid, new_password)
    return API.success("password updated")
