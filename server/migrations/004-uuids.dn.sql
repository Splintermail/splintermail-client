-- choose old-style column ids via auto_increment
create table temp (
    user_id int auto_increment,
    user_uuid binary(32) not null,
    primary key (user_id)
);
insert into temp (user_uuid) select x.user_uuid from (
    select user_uuid from accounts
) as x;

-- add user_id column to tables
alter table accounts add column user_id int(10) zerofill;
alter table aliases add column user_id int(10) zerofill;
alter table devices add column user_id int(10) zerofill;
alter table tokens add column user_id int(10) zerofill;

-- populate the user_id columns in tables
update accounts as z set user_id=(
    select user_id from temp where temp.user_uuid = z.user_uuid
);
update aliases as z set user_id=(
    select user_id from temp where temp.user_uuid = z.user_uuid
);
update devices as z set user_id=(
    select user_id from temp where temp.user_uuid = z.user_uuid
);
update tokens as z set user_id=(
    select user_id from temp where temp.user_uuid = z.user_uuid
);

-- modify user_id column in tables
alter table accounts modify column user_id int(10) zerofill not null;
alter table aliases modify column user_id int(10) zerofill not null;
alter table devices modify column user_id int(10) zerofill not null;
alter table tokens modify column user_id int(10) zerofill not null;

-- remove foreign key relations in non-accounts tables
alter table aliases drop constraint fk_aliases_user_uuid;
alter table devices drop constraint fk_devices_user_uuid;
alter table tokens drop constraint fk_tokens_user_uuid;

-- new primary key in accounts table
alter table accounts drop primary key;
alter table accounts add primary key (user_id);

-- drop the uuid columns in all tables
alter table accounts drop column user_uuid;
alter table aliases drop column user_uuid;
alter table devices drop column user_uuid;
alter table tokens drop column user_uuid;

-- recreate the deletions table in its previous form
drop table deletions;
create table deletions (
    email varchar(100) NOT NULL,
    server_id int(11) NOT NULL,
    state int(11) NOT NULL DEFAULT "0",
    primary key (email, server_id)
);

drop table temp;
