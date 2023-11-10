const lstr_t ZONE_NAME[] = {
    LSTR("user"), LSTR("splintermail"), LSTR("com")
};
const size_t ZONE_NAME_COUNT = sizeof(ZONE_NAME) / sizeof(*ZONE_NAME);

const uint32_t NS_TTL = 86400;

// A record //

const uint32_t A_TTL = 86400;

// NS record //

const lstr_t NS1_NAME[] = {
    LSTR("ns1"), LSTR("splintermail"), LSTR("com")
};
const lstr_t NS2_NAME[] = {
    LSTR("ns2"), LSTR("splintermail"), LSTR("com")
};
const size_t NS1_NAME_COUNT = sizeof(NS1_NAME) / sizeof(*NS1_NAME);
const size_t NS2_NAME_COUNT = sizeof(NS2_NAME) / sizeof(*NS2_NAME);


// TXT record //

const uint32_t TXT_TTL = 60;
const lstr_t TXT_NOTFOUND = LSTR("NOTFOUND");

// AAAA record //

const uint32_t AAAA_TTL = 86400;


// SOA record //

const size_t SOA_TTL = 86400;

// MNAME: primary source of data for this zone
const lstr_t MNAME[] = {
    LSTR("ns1"), LSTR("splintermail"), LSTR("com")
};
const size_t MNAME_COUNT = sizeof(MNAME) / sizeof(*MNAME);

// RNAME: email address of person responsible for this zone
const lstr_t RNAME[] = {
    LSTR("hostmaster"), LSTR("splintermail"), LSTR("com")
};
const size_t RNAME_COUNT = sizeof(RNAME) / sizeof(*RNAME);

// SERIAL, constant since it seems to be only useful for zone transfers
const uint32_t SERIAL = 20231108;
// REFRESH: 5 min
const uint32_t REFRESH = 300;
// RETRY: 1 min
const uint32_t RETRY = 60;
// EXPIRE: 1 day
const uint32_t EXPIRE = 86400;
// MINIMUM: 1 min
const uint32_t MINIMUM = 60;

// CAA record //

const uint32_t CAA_TTL = 60;
const uint8_t CAA_FLAGS = 128;  // critical bit is set
const lstr_t CAA_TAG = LSTR("issue");
const lstr_t CAA_VALUE = LSTR("letsencrypt.org");
