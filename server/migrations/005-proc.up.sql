-- create stored functions for converting to/from binary uuids and
-- file-system-safe identifiers (at this time, it's just hex encoding)
create function TO_FSID (user_uuid binary(32)) returns char(64) deterministic
    return LOWER(HEX(user_uuid));

create function TO_UUID (fsid char(64)) returns binary(32) deterministic
    return UNHEX(fsid);
