-- Right now we have emails spread around accounts.email/aliases/used_aliases.
-- The global uniqueness contstraint is much too complex to safely enforce.
--
-- Modify the schema to have a single table with a primary key of emails, and
-- let all email addresses hold a foreign relation to that table

-- -- I manually ran this one-time migration to fix the old random aliases which
-- -- had never been added to the used_aliases table:
-- insert into used_aliases (alias) select x.alias from (
--     select alias from aliases where alias not in (
--         select alias from used_aliases
--     ) and substring(alias, -22, 5) RLIKE '^[0-9]+$'
-- ) as x;


-- back-populate the paid column of aliases, as in the future we will have to
-- key off of that information for when to delete aliases from the emails table
update aliases set paid = if(alias in (select alias from used_aliases), 0, 1);

-- emails: all the known emails in a system.  Random aliases, when placed here,
-- never come out, even if the corresponding entry in the aliases table is
-- removed.  Account emails and paid aliases are removed when the corresponding
-- entry is removed.
create table emails (
    email varchar(100) NOT NULL,
    primary key (email)
);

-- populate emails from accounts
insert into emails (email) select x.email from (select email from accounts) as x;
-- populate emails from aliases
insert into emails (email) select x.alias from (select alias from aliases) as x;
-- populate emails from used_aliases (except the active aliases)
insert into emails (email) select x.alias from (
    select alias
    from used_aliases
    where alias not in (select alias from aliases)
) as x;

-- now all email addresses in the database must reference the emails table
-- this provides sql guarantees of uniqueness
alter table accounts add constraint fk_accounts_email foreign key (email) references emails (email);
alter table aliases add constraint fk_aliases_alias foreign key (alias) references emails (email);

-- the used_aliases table is no longer useful
drop table used_aliases;
