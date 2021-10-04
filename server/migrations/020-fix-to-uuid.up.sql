-- you can't use TRIM to strip just one 'F' from an fsid because fsids that
-- start with an 'F' will get over-trimmed.

-- drop the new definitions
drop function TO_UUID;
drop function TO_UUID_SOFT;

-- redefine the old functions using SUBSTRING instead of TRIM:
-- - create function TO_UUID (fsid char(44)) returns binary(32) DETERMINISTIC
-- -     return FROM_BASE64(REPLACE(CONCAT(TRIM(LEADING "F" FROM fsid), "="), '-', '/'));
create function TO_UUID (fsid char(44)) returns binary(32) DETERMINISTIC
    return FROM_BASE64(REPLACE(CONCAT(SUBSTRING(fsid from 2 for 43), "="), '-', '/'));

-- -create function TO_UUID_SOFT (fsid TEXT) returns binary(32) DETERMINISTIC
-- -    BEGIN
-- -        DECLARE EXIT HANDLER FOR SQLEXCEPTION, 1406
-- -        BEGIN
-- -            return NULL;
-- -        END;
-- -        return FROM_BASE64(REPLACE(CONCAT(TRIM(LEADING "F" FROM SUBSTRING(fsid FROM 1 FOR 44)), "="), '-', '/'));
-- -    END;
create function TO_UUID_SOFT (fsid TEXT) returns binary(32) DETERMINISTIC
    BEGIN
        DECLARE EXIT HANDLER FOR SQLEXCEPTION, 1406
        BEGIN
            return NULL;
        END;
        return FROM_BASE64(REPLACE(CONCAT(SUBSTRING(fsid FROM 2 FOR 43), "="), '-', '/'));
    END;
