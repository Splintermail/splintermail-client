-- track login state and csrf tokens in the database
--
-- this allows for seamless switching between servers, and also for the api
-- server to be able to authenticate requests based on session cookies without
-- actually reading the session at all.
--
-- One alternative considered was to just put php sessions in the database.
-- However, that strategy introduced a plethora of opportunities for
-- split-brains (either at the database level, or at the php session level)
-- since php sessions are just mutable blobs.
--
-- You could avoid split-brain by not letting either server edit the other
-- server's sessions, but then you didn't really buy yourself anything beyond
-- additional complexity.
--
-- Here's a good doc on that complexity, if you ever want it:
--      https://gist.github.com/mindplay-dk/623bdd50c1b4c0553cd3
--
-- A better strategy is to let php be php, and just track the login sessions
-- in a way that was specifically designed to be multi-master safe.
--
-- We can even just borrow the php session cookie identifier to make it easy.
--
-- The overall design is that each server only edits rows that belong to it.
-- Each server also runs its own GC to clean up expired sessions.
-- If server A wants to update the last_seen time of a session that only
-- server B has a row for, server A just duplicates that row, updating the
-- server_id and the last_seen time.  If the user logs out on one server, the
-- void bit is set on that server's row, and either server should respect it.

-- login and last_seen columns are timestamps in epoch time
create table sessions (
    session_id char(48) not null,
    server_id int(10) not null,
    user_uuid binary(32) not null,
    login bigint(20) zerofill not null,
    last_seen bigint(20) zerofill not null,
    -- if the user explicitly logs out, void this session everywhere
    void bit(1) default b'0',
    PRIMARY KEY (session_id, server_id)
);

-- csrf tokens; you get a new one for each visit to a csrf-protected page, but
-- any unexpired csrf token is valid.  Cycling them prevents certain attacks
-- like the BREACH attack.
create table csrf (
    csrf_id char(44) not null,
    server_id int(10) not null,
    session_id char(48) not null,
    created bigint(20) zerofill not null,
    PRIMARY KEY (csrf_id)
);
