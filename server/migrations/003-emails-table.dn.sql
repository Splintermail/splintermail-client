-- recreate the used_aliases table
create table used_aliases (
    alias varchar(100) NOT NULL,
    primary key (alias)
);

-- fill used_aliases with all emails that are not accounts or paid aliases
insert into used_aliases (alias) select x.email from (
    select email
    from emails
    where email not in (select alias from aliases where paid)
    and email not in (select email from accounts)
) as x;

-- drop foreign keys
alter table accounts drop constraint fk_accounts_email;
alter table aliases drop constraint fk_aliases_alias;

drop table emails;
