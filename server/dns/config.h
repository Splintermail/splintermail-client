// SOA record //

const lstr_t SOA_NAME[] = {
    LSTR("user"), LSTR("splintermail"), LSTR("com")
};
const size_t SOA_NAME_COUNT = sizeof(SOA_NAME) / sizeof(*SOA_NAME);
const size_t SOA_TTL = 86400;

// MNAME: primary source of data for this zone
const lstr_t MNAME[] = {
    LSTR("userns1"), LSTR("splintermail"), LSTR("com")
};
const size_t MNAME_COUNT = sizeof(MNAME) / sizeof(*MNAME);

// RNAME: email address of person responsible for this zone
const lstr_t RNAME[] = {
    LSTR("hostmaster"), LSTR("splintermail"), LSTR("com")
};
const size_t RNAME_COUNT = sizeof(RNAME) / sizeof(*RNAME);

// SERIAL, always 0 since it seems to be only useful for zone transfers
const uint32_t SERIAL = 20220806;
// REFRESH: 5 min
const uint32_t REFRESH = 300;
// RETRY: 1 min
const uint32_t RETRY = 60;
// EXPIRE: 1 day
const uint32_t EXPIRE = 86400;
// MINIMUM: 1 min
const uint32_t MINIMUM = 60;
