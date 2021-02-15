-- create stored functions for converting to/from binary uuids and
-- file-system-safe identifiers
create function TO_FSID (user_uuid binary(32)) returns char(44) deterministic
    return REPLACE(TO_BASE64(user_uuid), '/', '-');

create function TO_UUID (fsid char(44)) returns binary(32) deterministic
    return FROM_BASE64(REPLACE(fsid, '-', '/'));
