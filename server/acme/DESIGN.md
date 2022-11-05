# Per-installation TLS Certs

## Diagram

                         4 order   _________
                  +-------------->|         |
                  |      5 token  | ZeroSSL |
                  | +-------------|_________|
     _____        | |    8 csr      ^ |
    |     |       | | +-------------+ |
    | DNS |       | | |  9 cert       |
    |_____|       | | | +-------------+
      ^ |       _ | | | | _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _
      | |      |  | | | |                        server  |
      | | 7 ok    |_v_|_v_ 10 cert ____ 11 cert ______
      | +------->|        |------>|    |------>|      |  |
      | 6 token  |  ACME  | 3 csr | DB | 2 csr | REST |
      +----------|________|<------|____|<------|______|  |
                                                 ^  |
               |_ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _|_ | _ _|
                                                 |  |
                                           1 csr |  | 12 cert
                                               __|__v__
                                              |        |
                                              | client |
                                              |________|

## 12 steps to valid certs

0. user device generates keypair and certificate signing request (CSR)
1. user device POSTs a CSR to the REST API
2. REST server puts it in the database, and maybe kicks ACME to check database
3. ACME pulls the CSR from the database
4. ACME POSTs an order with ZeroSSL for associated subdomain
5. ZeroSSL responds with a token
6. ACME pushes token to DNS server
7. DNS server acknowledges that token is configured
8. ACME POSTs the CSR to the challenge URL
9. ZeroSSL verifies the challenge, then responds with a signed certificate
10. ACME puts the signed certificate in the DB
11. REST pulls signed certificate out of DB and ...
12. ... REST responds to user request with the signed certificate

## Service design

The ACME service and the REST service could technically be combined, but are
different enough it doesn't make too much sense for them to be combined.  The
only cost of separating them is adding the ability for each one to kick the
other when it updates the database.

Another reason that the ACME service and REST service make sense as separate
entities is that the REST API primarily servers customer requests, while the
ACME service is more of a client to other services.  Technically the ACME
responsibilities could be shared between the active server and the inactive
server, though the REST service can't operate that way.

The DNS service is on its own server in the cloud.  This is to avoid the normal
cloud-server traffic relay cost.  It is feasible because it doesn't violate our
standard "no secrets on cloud hardware" rule, since the only secrets it handles
are ACME tokens (not very sensitive, and not long-lived) and VPN keys.  Care
should be taken that the DNS traffic back to the ACME service is limited to
only the ports it absolutely needs to talk to the ACME service.

## Lease system

Even though there are multiple splintermail servers, only one ACME service
should be advancing the state machine for a given certificate active at a time.
This implies a lease system of some sort, where a service needs positive
confirmation that it _should_ be operating in order to operate.  That way, if a
server loses its connection to the rest of the Splintermail services, its ACME
service will automatically shut down at a coordinated time, and the other ACME
service on the other server can take over at that coordinated time.

In the future, perhaps a sharded scheme could developed where each user's CSR
belongs to one of N shards, leases are granted to whichever M ACME services
appear to be healthy.  This would also require a new strategy for triggering
the REST API server to respond to the client that a certificate is ready.

## ACME/DNS coordination

DNS normally expects is to have at least 2 DNS servers for a domain.  For now,
we may decide to only run one.

But in the future, if/when we expand to multiple DNS servers, we'll need a
mechanism by which the ACME service can continue if one or more DNS servers are
unresponsive.  Otherwise, the system would fail if any of N servers failed,
which is worse than just having a single server.

What I plan for that is that a DNS server responds with errors or redirects or
something to `_acme_challenge` queries for shards where it has not received an
update for that shard after X seconds.  Meanwhile, the ACME service for that
shard will attempt to update all DNS servers before POSTing the CSR to the
challenge URL, but it is allowed to proceed after X seconds if a DNS server is
unresponsive to synchronization attempts.  Presumably, ZeroSSL's servers should
do normal DNS failure handling and arrive at the functional DNS server to
validate the challenge.

After a particular DNS server becomes unresponsive, the ACME service need not
wait X seconds again; it should continue to try to contact that DNS server
with "you are not synchronized" messages.  When it becomes responsive again,
the DNS server continues to respond as before until resynchronization is
complete, and then the DNS server and the ACME service behave as normal again.
