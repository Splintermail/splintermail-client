-- restore yet-unused columns
alter table accounts add column max_primary_aliases int(10) unsigned NOT NULL DEFAULT "0";
alter table accounts add column spam_sorting bit(1) DEFAULT b'1';
alter table accounts add column future_bill float DEFAULT "1";
alter table accounts add column dev_updates bit(1) DEFAULT b'0';
alter table accounts add column spamassassin bit(1) DEFAULT b'1';
alter table accounts add column max_bytes_storage bigint(20) unsigned DEFAULT "0";
alter table accounts add column next_bill float DEFAULT "1";
alter table accounts add column bitcoin_addr varchar(40);
alter table accounts add column discount float DEFAULT "0";
alter table accounts add column invite_code varchar(36);
alter table accounts add column free_trial_active bit(1) DEFAULT b'1';
alter table accounts add column bytes_storage bigint(20) unsigned DEFAULT "0";
alter table accounts add column balance float DEFAULT "0";

-- restore static count columns
alter table accounts add column num_devices int(10) unsigned NOT NULL DEFAULT "0";
alter table accounts add column num_primary_aliases int(10) unsigned NOT NULL DEFAULT "0";

-- populate those columns
update accounts set num_devices=(
    select COUNT(*) from devices where devices.user_uuid=accounts.user_uuid
);
update accounts set num_random_aliases=(
    select COUNT(*) from aliases where aliases.user_uuid=accounts.user_uuid and aliases.paid=false
);
update accounts set num_primary_aliases=(
    select COUNT(*) from aliases where aliases.user_uuid=accounts.user_uuid and aliases.paid=true
);

-- never run this ever; there's no way for users to recover their accounts!
-- alter table accounts modify password varchar(106);

alter table accounts modify daily_msg_limit smallint(5) unsigned NOT NULL DEFAULT "100";
alter table accounts modify msg_count smallint(5) unsigned NOT NULL DEFAULT "0";
