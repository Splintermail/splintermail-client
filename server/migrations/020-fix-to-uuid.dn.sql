-- drop the new definitions
drop function TO_UUID;
drop function TO_UUID_SOFT;

-- restore the old definitions
create function TO_UUID (fsid char(44)) returns binary(32) DETERMINISTIC
    return FROM_BASE64(REPLACE(CONCAT(TRIM(LEADING "F" FROM fsid), "="), '-', '/'));

-- If you need to convert an arbitrary string which *might* be an fsid.
-- The SQLEXCEPTION guards against bad base64 strings.
-- We re-embed the whole function body without the char(44) limitation to
-- handle strings which are too long.
create function TO_UUID_SOFT (fsid TEXT) returns binary(32) DETERMINISTIC
    BEGIN
        DECLARE EXIT HANDLER FOR SQLEXCEPTION, 1406
        BEGIN
            return NULL;
        END;
        return FROM_BASE64(REPLACE(CONCAT(TRIM(LEADING "F" FROM SUBSTRING(fsid FROM 1 FOR 44)), "="), '-', '/'));
    END;
