-- public_key column used to be a BLOB but it is actually just filled with
-- 800-ascii-character entries, so let's optimize that a bit

-- may truncate data, but only in the case of invalid public keys
alter table devices change public_key public_key varchar(1024);
