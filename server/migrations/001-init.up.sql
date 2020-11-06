-- tables as of 5 Nov 2020
-- content generated from applying the old manage_sql code to an empty database
create table devices (
    nonce bigint(20) unsigned zerofill NOT NULL DEFAULT "00000000000000000000",
    public_key blob NOT NULL,
    user_id int(10) unsigned zerofill NOT NULL,
    fingerprint binary(64) NOT NULL,
    primary key (fingerprint)
);
create table deletions (
    email varchar(100) NOT NULL,
    server_id int(11) NOT NULL,
    state int(11) NOT NULL DEFAULT "0",
    primary key (email, server_id)
);
create table aliases (
    alias varchar(100) NOT NULL,
    temp bit(1) DEFAULT b'0',
    expires date,
    user_id int(10) unsigned zerofill NOT NULL,
    paid bit(1) DEFAULT b'0',
    primary key (alias)
);
create table tokens (
    secret char(44) NOT NULL,
    nonce bigint(20) unsigned zerofill NOT NULL DEFAULT "00000000000000000000",
    user_id int(10) unsigned zerofill NOT NULL,
    token int(10) unsigned zerofill NOT NULL,
    primary key (token)
);
create table used_aliases (
    alias varchar(100) NOT NULL,
    primary key (alias)
);
create table virtual_domains (
    name varchar(50) NOT NULL,
    id int(11) NOT NULL auto_increment,
    primary key (id)
);
create table accounts (
    max_primary_aliases int(10) unsigned NOT NULL DEFAULT "0",
    num_devices int(10) unsigned NOT NULL DEFAULT "0",
    user_id int(10) unsigned zerofill NOT NULL,
    spam_sorting bit(1) DEFAULT b'1',
    future_bill float DEFAULT "1",
    dev_updates bit(1) DEFAULT b'0',
    num_random_aliases int(10) unsigned NOT NULL DEFAULT "0",
    email varchar(100) NOT NULL UNIQUE,
    spamassassin bit(1) DEFAULT b'1',
    max_bytes_storage bigint(20) unsigned DEFAULT "0",
    next_bill float DEFAULT "1",
    bitcoin_addr varchar(40),
    daily_msg_limit smallint(5) unsigned NOT NULL DEFAULT "100",
    msg_count smallint(5) unsigned NOT NULL DEFAULT "0",
    limit_msg_sent bit(1) NOT NULL DEFAULT b'0',
    num_primary_aliases int(10) unsigned NOT NULL DEFAULT "0",
    discount float DEFAULT "0",
    domain_id int(11) NOT NULL,
    invite_code varchar(36),
    free_trial_active bit(1) DEFAULT b'1',
    bytes_storage bigint(20) unsigned DEFAULT "0",
    balance float DEFAULT "0",
    password varchar(106) NOT NULL,
    primary key (user_id)
);
