-- Installations is like tokens but with additional information.  It is a
-- separate table because normal tokens can't be used to authorize
-- installation-related actions, and installation tokens can't be used for
-- anything related to normal accounts.
--
-- However, since the acme provider needs a contact email, and since we need a
-- way to ensure installations are not created totally willy-nilly, each
-- installation is tied to an account, which must be provided at setup time.
create table installations (
    -- some parts are the same as tokens:
    secret char(44),
    nonce bigint(20) unsigned zerofill not null default 0,
    token int(10) unsigned zerofill not null unique,
    user_uuid binary(32) not null,
    -- other parts are unique to installations:
    inst_uuid binary(32) primary key,
    subdomain varchar(63) not null unique,
    challenge varchar(255),
    email varchar(100) not null unique,
    constraint fk_installations_accounts foreign key (user_uuid)
        references accounts (user_uuid),
    -- we'll want to be able to search by token as well
    index (token)
);
