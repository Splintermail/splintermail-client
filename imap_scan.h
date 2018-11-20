#ifndef IMAP_SCAN_H
#define IMAP_SCAN_H

#include "common.h"

// the scanner can only return these values
typedef enum {
    SCAN_STATUS_OK,
    SCAN_STATUS_ERROR,
    SCAN_STATUS_MORE,
} scan_status_t;

typedef struct {
    dstr_t bytes;
    char bytes_buffer[256];
    // const char* limit;
    // continued scan start position
    const char* cursor;
    // continued scan backup position
    const char* marker;
    // clean scan start position
    const char* start;
    // previous scanner start position (start of the last token)
    const char* old_start;
    // type of last token
    int num;
    // part of continued scan state
    int state;
    bool accept;
    char yych;
    // for automatic continue handling
    bool continuing;
} imap_scanner_t;

scan_status_t imap_scan(imap_scanner_t *scanner);

#endif // IMAP_SCAN_H
