-- drop columns I had plans for but never used
alter table accounts drop column max_primary_aliases;
alter table accounts drop column spam_sorting;
alter table accounts drop column future_bill;
alter table accounts drop column dev_updates;
alter table accounts drop column spamassassin;
alter table accounts drop column max_bytes_storage;
alter table accounts drop column next_bill;
alter table accounts drop column bitcoin_addr;
alter table accounts drop column discount;
alter table accounts drop column invite_code;
alter table accounts drop column free_trial_active;
alter table accounts drop column bytes_storage;
alter table accounts drop column balance;

-- drop static counts in favor of runtime counting
-- (except num_random_aliases, since it counts all aliases ever)
alter table accounts drop column num_devices;
alter table accounts drop column num_primary_aliases;

-- also update the limit for password hashes to support >5000 rounds of sha512
alter table accounts modify password varchar(128);

-- why on earth did I use smallint??
-- use unsigned int to make the C code easier to write
alter table accounts modify daily_msg_limit int(10) unsigned NOT NULL DEFAULT "100";
alter table accounts modify msg_count int(10) unsigned NOT NULL DEFAULT "0";
