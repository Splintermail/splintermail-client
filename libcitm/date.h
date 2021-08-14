// always returns a date, falling back to a static string for Jan 1, 1970
// if epoch is (time_t)-1 this function will call dtime() as well
const char *get_date_field(char *buf, size_t len, time_t epoch);

// returns 1 Jan 1970 on error
// if epoch is (time_t)-1 this function will call dtime() as well
imap_time_t imap_time_now(time_t epoch);
