#ifndef LIBDNS_H
#define LIBDNS_H

#include "libdstr/libdstr.h"
#include "liburl/liburl.h"
#include "libduv/libduv.h"
#include "server/libkvpsync/libkvpsync.h"

/* Why write a custom dns server?

   - Need dynamic updates.  And when we support DNSSEC, we want the signing key
     to not live on the machine running DNS.  DNS will be in a cloud machine,
     and we do not put keys on cloud machines.

   - RFC-defined wildcard behavior is not really correct for us, so it's far
     easier to hand-write the wildcard logic.

   - One record we want to emit is dynamic.  Existing solutions which are
     complete enough to allow plugin behavior are also crazy complex.

       - Bind has a crappy plugin interface; also their wildcard RRL is broken.

       - PowerDNS has a plugin architecture that would work, but I am unwilling
         to introduce C++ into the splintermail tech stack, just so I can
         include a heavyweight and complex dns server in production that I
         don't really want anyway.

       - DJB's tinydns is similar in spirit to our dns server, but is not
         capable enough to meet our needs.

   - Also it was fun to write. */

#include "server/dns/membuf.h"
#include "server/dns/parse.h"
#include "server/dns/write.h"
#include "server/dns/dns.h"
#include "server/dns/rrl.h"

#endif // LIBDNS_H
