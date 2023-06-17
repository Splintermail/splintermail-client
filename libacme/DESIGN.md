# Per-installation TLS Certs

## Diagram

             1 order   _____________
      +-------------->|             |                  _____
      |      2 token  | letsencrypt |                 |     |
      | +-------------|_____________|                 | DNS |
      | |    11 csr     ^ |                           |_____|
      | | +-------------+ |                             ^ |
      | | |  12 cert      |                             | |
      | | | +-------------+                     7 token | | 8 ok
      | | | |        _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _|_|_ _
      | | | |       | server                            | |   |
      | | | |                                           | |
      | | | |       |         5 long-poll request       | |   |
      | | | |           +-----------------------------+ | |
      | | | |       |   |     9 long-poll response    | | |   |
      | | | |           | +-------------------------+ | | |
      | | | |       |   | |                         | | | |   |
      | | | |          _|_v__         ____         _|_v_|_v_
      | | | |       | |      |4 token|    |6 token|         | |
      | | | |         | REST |------>| DB |------>| kvpsend |
      | | | |       | |______|       |____|       |_________| |
      | | | |           ^ |
      | | | |       |_ _|_|_ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _|
      | | | |           | |
      | | | |   3 token | | 10 ok
     _|_v_|_v___________|_v_____
    |                           |
    |        user device        |
    |___________________________|


## 12 steps to valid certs

Step 0: user device generates keypair and certificate signing request (CSR).

1. user device POSTs letsencrypt order
2. letsencrypt responds with a token
3. user device POSTs token to REST API with
4. REST API stores in the database, and maybe kicks kvpsend
5. REST API begins long-poll against kvpsync service
6. kvpsend reads token from the database
7. kvpsend pushes token to dns servers
8. dns server acknowledges the token
9. kvpsend responds to REST API long-poll request
10. REST API responds OK to user device
11. user device POSTs CSR to letsencrypt
12. letsencrypt responds with a certificate

## Service design

The kvpsend service and the REST service could technically be combined, but are
different enough it doesn't make too much sense for them to be combined.  The
only cost of separating them is adding the ability for each one to kick the
other when it updates the database.

The DNS service is on its own server in the cloud.  This is to avoid the normal
cloud-server traffic relay cost and to reduce latency.  It is feasible because
it doesn't violate our standard "no secrets on cloud hardware" rule, since the
only secrets it handles are ACME tokens (not very sensitive, and not
long-lived) and VPN keys.  Care should be taken that the DNS traffic back to
the kvpsend service is limited to only the ports it absolutely needs to talk to
the kpvsend service.

For reliability, we will have two dns servers, and each splintermail server
will have a kvpsync service.  There will be four parallel kvpsync protocol
pairs (two senders sending to each of two receivers) occurring simultaneously,
and as long as one sender and one receiver are both alive the service should
continue mostly unaffected.

## Inter-service communication

We want high responsiveness for the end-user.  We can accomplish this with long
polling.

The REST API has complex execution logic, which makes a wakeup-then-check-the-
database strategy difficult to implement, even though that would be preferable
since the REST API on a node would be fully capable with or without the kvpsend
service actively running on that node.  That strategy would be difficult
because it would be hard to guarantee that the wakeup was being sent to the
correct REST API worker.  In fact, it might be necessary to just have a
separate wakeup watcher... which is easier to just build into the kvpsend
service since that's where wakeups will originate from anyway.

Therefore, the easiest way to implement long-polling will be for the REST API
to do authentication, but effectively just proxy the actual long-poll logic to
kvpsend.

I've also thought about a mysql trigger-based wakeups.  The wakeups should work
even across replicas if you use statement-based triggers.  But the triggers
run before a transaction is committed, so you would need to manually invoke
the triggering statement.  You would write the kvpsync request into a kvpsync
table, then write an arbitrary change to a wakeup table, which has a trigger
statement that sends a udp packet.  This is the best way I can think of for
cross-node triggers based on database replication.

## Long-polling overhead

Allowing long-polling for this endpoint should not introduce a significant
overhead to the server.  If there were a million installations, each updating
their certificate every 60 days, and if each took an average of 1 minute to
update, that would average less than 12 long-polling connections at a time.

## ACME Step-by-step

0. if certificate exists and doesn't need updating, sleep until it does.

1. if an account is is on file, load it and skip to 4.

2. if a jwk is on file, load it and skip to 5.

3. generate a jwk, write it to file

4. create an account with the jwk, write it to file

5. list orders.  If one is valid, take the cert and skip to 13.  If one is
   processing, skip to 13.

6. Create a new order.

7. get authz in order to view challenge token.  If status is not "valid" skip
   to 9.

8. set challenge token with rest api (blocks until challenge is ready).

9. post to challenge.

10. poll for authorization status to become "valid" or "invalid".  If invalid,
    log reason and return to step 6.

11. generate key, write to file.

12. generate crl, finalize order.

13. poll for order.status == "valid"

14. write certificate to file.

15. delete challenge with rest api

16. return to step 0.
