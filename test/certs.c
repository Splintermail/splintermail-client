#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"

#include "test/certs.h"

#define GOOD_CA_KEY \
"-----BEGIN PRIVATE KEY-----\n" \
"MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDBaMv4u0BSTCjK\n" \
"EXQQGKyjFrBKa4As21RL0pVQIdh+GdJaFpiZhxe6SBEPUnSas9odlZIQhmo5g1z+\n" \
"l9BmxH19DNlbTOC/14M1wLnVn1/OWxFsQf2SCfpvvqxDX1tsA5r+l+kdiSqim55o\n" \
"RpqAwzKzgw1Asg+YRVkSAuVe19LDqhmOTT9dW2fyHEGyjObInq55TOWFk2/7mrKx\n" \
"Rvn4SmdoCOOYNhtCkaqS+yM/pH4TvkItW3V3a72ydSF5jyUKGOBOK6fWUrSmuMYK\n" \
"rN6OQlgxeD/M+nyg1uGihJDc0zxyzKQrBqqTBnf0zh61ziLia8hZYWZ+4y1KTTbk\n" \
"sXimNUqpAgMBAAECggEAGm2iw5X4u9Ym07fPU3y/qFhrFfw7C0YcLnEz0HuTfOWz\n" \
"4fYI+5+jXSPIWv7iKpqNYTIFP8dSQBIkdLCTfFt4p0wIbmqiomxFRGTVr+xjd8vn\n" \
"ZVLeHqTI6RiVqu4ejLOwa/4fj1blMcuQeYC+T3580N1FRBQgv+an/WdSZuOYa/52\n" \
"09S1emweH0bbiytBLRYSg1xBCzMYqbrPPTQtmN94tpg1ttjHvpkA5f9mg0toozb/\n" \
"cw5l5n5aIdlzWxURZumBSrqBavUiXSDcdswhsGQulRZ+FdXGDER8FmR5qVJSxjTG\n" \
"edY7vb2pf9Wel54mkHia8WYl0Jnfy7f+2OAc9QoZxQKBgQD7cQxqNUFO8WpVH1vi\n" \
"brQEw2KdGKUGXiVl5dIjOx2S8FKAcK16VEr9peFhyT5SKv9QUs+tz8YFZrr662u0\n" \
"NQ/pfTs7iGcweYtpWPRQxO0+XPkuwk59dH/CrnXMwtGKpwCYCWTYXEm9DLPmuzyu\n" \
"8rmAwTs9GwreF3MblxuZi43JlQKBgQDE6msJ/sUolAm5ze9+a6XENIR7T5rHf5YE\n" \
"qcxOH0oZo1v1iZ1LS8aUQYPwteY4TEQleO1ch52yoUF+vJutJoJqYL4bQzD+dImD\n" \
"9A98bTVtfsqoqactFtefKu2Ggq1EuR8X5hDkX8pSxA0kdFq3+d5OMp00Uw1VUha6\n" \
"0eR19ye/xQKBgQD33Kz8VZsXkuvbFZqiT7atq12etxiWemArXq4ThMbWfokVi/22\n" \
"xTlXaRoQJy2EGlase7W2BUeTM33GtCPr9RLGfGjTetcd9fLz2nic1kN4YnZRHcH5\n" \
"8cmvxALj4nhlUdNIDJqYngEil08QeCqN2z/6KIGY7vt7i2oxHHhcmDEhMQKBgQCe\n" \
"sMfIS0/6LqtT2Llr6Tay0xv9AB8dLR3p7ijewGqIFNVUIC3p80w6SD4bK4w3zIaS\n" \
"qEypaAVXYosUpszSkplBP2uVLuX5+lKkS3h7bstCzY5mtCostR7Zf8/hucgG/SUo\n" \
"rljooqW7Y5Gv0jEV2MElxbZY56F0I+57ahEyXBblfQKBgDftansHZYLxqz9qSi1y\n" \
"LFooeDyOhAsJbopCb3noFoV7vAWhYQacVrHWD6E8lTiPQRsvX939szUyd/UaYZky\n" \
"OrmSjCPGkV8Q0O21QX6hTU0y8d5RmQMhjCJ+c4JpJw4BPPvAmLMDewA44pz4vyJy\n" \
"SBByx19pqXUcX47ipA65svdy\n" \
"-----END PRIVATE KEY-----\n"

#define GOOD_CA_CERT \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDnzCCAoegAwIBAgIUUCxrN+7lz+ZHEYh2LWsAXwBI+m8wDQYJKoZIhvcNAQEN\n" \
"BQAwVzELMAkGA1UEBhMCVVMxHjAcBgNVBAoMFVRydXN0ZWQgVGVzdCBMb2NhbCBD\n" \
"QTEMMAoGA1UECwwDT3JnMRowGAYDVQQDDBF0cnVzdGVkLmxvY2FsaG9zdDAeFw0y\n" \
"MzA5MTYxNjI0NDJaFw0zMzA5MTMxNjI0NDJaMFcxCzAJBgNVBAYTAlVTMR4wHAYD\n" \
"VQQKDBVUcnVzdGVkIFRlc3QgTG9jYWwgQ0ExDDAKBgNVBAsMA09yZzEaMBgGA1UE\n" \
"AwwRdHJ1c3RlZC5sb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK\n" \
"AoIBAQDBaMv4u0BSTCjKEXQQGKyjFrBKa4As21RL0pVQIdh+GdJaFpiZhxe6SBEP\n" \
"UnSas9odlZIQhmo5g1z+l9BmxH19DNlbTOC/14M1wLnVn1/OWxFsQf2SCfpvvqxD\n" \
"X1tsA5r+l+kdiSqim55oRpqAwzKzgw1Asg+YRVkSAuVe19LDqhmOTT9dW2fyHEGy\n" \
"jObInq55TOWFk2/7mrKxRvn4SmdoCOOYNhtCkaqS+yM/pH4TvkItW3V3a72ydSF5\n" \
"jyUKGOBOK6fWUrSmuMYKrN6OQlgxeD/M+nyg1uGihJDc0zxyzKQrBqqTBnf0zh61\n" \
"ziLia8hZYWZ+4y1KTTbksXimNUqpAgMBAAGjYzBhMB0GA1UdDgQWBBT7eDwBpUMr\n" \
"WJaGFUQYPSolxn83nzAfBgNVHSMEGDAWgBT7eDwBpUMrWJaGFUQYPSolxn83nzAP\n" \
"BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjANBgkqhkiG9w0BAQ0FAAOC\n" \
"AQEAcj33duPrhVAiJNliP0YXC+n5VtfIaQIO8vZm+7szFrHxgSxN2olpP6dGO+XF\n" \
"IOrBi4DN9hsxZPCX9tmNa6Zodgq1Po70MwZg5BaDlWHlNRS3jazRiTPLUuopa90C\n" \
"fSchlyjMm9l1EttqocmpEqfRb/KBFb2DnDiX/O+f9AwBnxkyoEyek/s8pq1YGR3n\n" \
"w22fnff7MSRHs+2jxVpDR/+CXmRRPXoWedJZ/z2dPzh4jyPcsR66TsnKS08R9kfb\n" \
"itBdw5M5wnGvEUr3psGDX6DNXBy4bFqc9T2Cc513B0z8iXTHO3IvpX9JOdZDo+o3\n" \
"WYkBr4O4j2MsQNddUCLYNN7qeQ==\n" \
"-----END CERTIFICATE-----\n"

#define GOOD_127_0_0_1_KEY \
"-----BEGIN PRIVATE KEY-----\n" \
"MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDFKSD5K2xXQAWT\n" \
"8lNKRcrPqG/PmK5nOIPruAl+UX81Wwejc4bKGnGUvoGyVZ8U8cT9RbmVK+4gKT+t\n" \
"VmPaiGP2q2X1+5Ewy1y1JHP/v4uWUpUSUf/zrGJ9QV1h25ELBaDqn6hOQjITsQlv\n" \
"lejEGRrpi5UJdk4yUpnhi634SyuBAV6cGoBktV4NckBKeV74dUeXRJuop6+oEe/b\n" \
"kw7rCU0iYVT1hTMYCcJewCd1oiy5AGKgnFEu5x3KYkrBZKMLctWUH3FQ846kk1xQ\n" \
"1STua+cLJol/AK6BdWPqbv57HP80puTS+FQvlcjUcFlbUl1yHkAmJlJKj97GXwGd\n" \
"N2UAMN0BAgMBAAECggEAAYpMGK94UjGjzgP1KvFs5o+pcgMgfBnegqXV5taDMf6I\n" \
"0JvonPZvMBuja0o1xfJjGezTlrRnqNEU7U5BaW1QO2rDmSP6OoH1ErEJ+BrO/eBh\n" \
"w9NhvpmadVW+dX0RWd9LIGhZaIUqCXAsLIzGpz4WXyYWirbjrLsjSrAxYnkgDB7u\n" \
"FOoLFVZyuX6bMRibqAI9spX6mGzV0t1mPYF4Bwt+2pGnt7pTaDNq6LUeOMdCnxCk\n" \
"q1CHJWe1NgjV48Vu9nAPU0RPHbNnNGho/ZoeDN5T02sOGjQciWWzlh6ZECDYlide\n" \
"4ciPSTqeYMdLrOVWK7QWLqPQfHZAnwLhBta4HxDqgQKBgQDp78tCwCbTH14LhV8K\n" \
"sfAAQiEzT0KN4D2tj/gvLiDA4AAbX3yns/43TGhRDT89JaucwhBUatceNXQQ9tK2\n" \
"97hUo/gMTf6Fu80yljOCfrv5T3v8XOOyKS3kC5MGR1mrsNa4FGtA+hk1AwRF5ubj\n" \
"dRbvWquiENnhFWOBiKyozOsEgQKBgQDXwWh9P4lT5cTD/OGbTWeBkfuHrgv/4tIF\n" \
"0QqryonCHcGnBktK6OCLgVWsbydQttZSaJhf8cw56pdEeAiWiDb8u8mW6EhE+qus\n" \
"aZvb4CvmUCOH6/H7FiU7jvdxKTnYZ2RS50cEflygvryhTZN87+84JjEwOddzc5zD\n" \
"HHRZC5eYgQKBgFqjPe2u5VwHWzi7hAdwybxP4u0uWGr/5uXIUjB1fts77s6sQG/5\n" \
"nRVv1TsSFt5qKgOibRFIE/DwZPftqdaaRCEHJqQd0++cw/RFnc+BqvL9iNxOYSo2\n" \
"KkGg+mYEXwnWilroDmYugHW5pX4v6GVYiHpUdwtj3AcJcnRNsBf2zVgBAoGBANFd\n" \
"kXPY7/5F/UbiYls+Ja8KZjcnoeVRydRzIhPKmxuPnJfj97Rdf2hgvS5zPoeIpTma\n" \
"3iJclX2uM9PxVwQccp+V6u1Rwq+NQIIyiVg4lOEN6yy+vTaWKtM8zMk25DcnPadl\n" \
"XvjxVhl2fb1ZdkHK4yxtg1X1CymxivGa7xxpr1YBAoGBANL5LDi0mjqLRbkPRl0u\n" \
"20tM6TaRjjhnS3P359WaDfylrhdhrAkoAXkJ2AOln7zCS/TH26ZoxJuBFCzjz766\n" \
"vWtyUZVa3lEZwJRLzQW2MQ0Her0D+ctJbuQKjkzE4G5LM3qCc08VTYZEbHSrGEqN\n" \
"yT8dsp3fujkpeAAHsbVt64Ed\n" \
"-----END PRIVATE KEY-----\n"

#define GOOD_127_0_0_1_CERT \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDuTCCAqGgAwIBAgIUV1WBsOZyh6uhB6zumortWCjBnMgwDQYJKoZIhvcNAQEN\n" \
"BQAwVzELMAkGA1UEBhMCVVMxHjAcBgNVBAoMFVRydXN0ZWQgVGVzdCBMb2NhbCBD\n" \
"QTEMMAoGA1UECwwDT3JnMRowGAYDVQQDDBF0cnVzdGVkLmxvY2FsaG9zdDAeFw0y\n" \
"MzA5MTYxNjI0NDNaFw0zMzA5MTMxNjI0NDNaMEgxCzAJBgNVBAYTAlVTMRcwFQYD\n" \
"VQQKDA5Hb29kIFRlc3QgQ2VydDEMMAoGA1UECwwDT3JnMRIwEAYDVQQDDAkxMjcu\n" \
"MC4wLjEwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDFKSD5K2xXQAWT\n" \
"8lNKRcrPqG/PmK5nOIPruAl+UX81Wwejc4bKGnGUvoGyVZ8U8cT9RbmVK+4gKT+t\n" \
"VmPaiGP2q2X1+5Ewy1y1JHP/v4uWUpUSUf/zrGJ9QV1h25ELBaDqn6hOQjITsQlv\n" \
"lejEGRrpi5UJdk4yUpnhi634SyuBAV6cGoBktV4NckBKeV74dUeXRJuop6+oEe/b\n" \
"kw7rCU0iYVT1hTMYCcJewCd1oiy5AGKgnFEu5x3KYkrBZKMLctWUH3FQ846kk1xQ\n" \
"1STua+cLJol/AK6BdWPqbv57HP80puTS+FQvlcjUcFlbUl1yHkAmJlJKj97GXwGd\n" \
"N2UAMN0BAgMBAAGjgYswgYgwCQYDVR0TBAIwADALBgNVHQ8EBAMCBaAwHQYDVR0l\n" \
"BBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMA8GA1UdEQQIMAaHBH8AAAEwHQYDVR0O\n" \
"BBYEFMXwnN8ua1Y6fcU8v5s6F/fx9wh+MB8GA1UdIwQYMBaAFPt4PAGlQytYloYV\n" \
"RBg9KiXGfzefMA0GCSqGSIb3DQEBDQUAA4IBAQCpjjKqIuuBKO+b1///vS3G469u\n" \
"U8dh6HMzxwU7RW3jQ/uhxI509TA4F2JEq9RVn3G3W43lGOBll8hTz8P8G+QxkROY\n" \
"Y7DEPHQNThJBOp/VuCTBHb7YyaMBL4JRjQkXiTzjf0BFJ1chf56E9O6ewAeu+Tl4\n" \
"U6iFHKVixb0zHDsU51xaqtyVpV3Spz87MbbGNmoFKpztyyilPWJG31cd7X0hgLp+\n" \
"AyzCjX7f9uMSscNvJM3oaoPumRpsJybxcGJRZ82jQB5xBibKGR3UXm8A1G+srYdy\n" \
"qbn9rjHZFfS3jOUau8GnjLSKuf7PQC+yD++Y0pDXkFDh4NegpbwVSh2R4YKq\n" \
"-----END CERTIFICATE-----\n"

#define GOOD_NOBODY_KEY \
"-----BEGIN PRIVATE KEY-----\n" \
"MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCIKJhpLpHS2h+o\n" \
"nfcySnUvvjUl/te8nPhpv/q37Vbrm40R7mtMXLa/4dfHJ9dxyQZg3JJrrC/BB/qT\n" \
"uw1lNzZ/rn0wYBOvYOMLEYD2N91k/HGRu3MYbpo1WKKLuI3LGg3z1KXKXQ77blku\n" \
"uWKnZN4wfx/g2iD6HtXGMNfS2eHr1Vmn7zbWPak1W0o2JkJx4OMlcFVy/BNp9HfS\n" \
"RsgtJ0gK6TOA0rl4n65SYUuz2120LMlvj8QGHu0HPQx4JrrRzCYvEJZtl03qhnS0\n" \
"OvxXk2WcEiJ4Ro9migJXi4i2ymPUPoigbmd1taDaISY6qLuNA/b9ddOI0qMKaqzx\n" \
"uPYfgmwVAgMBAAECggEAByWaWmsHXHn2j3M7NniSDh99yrDFwjC7GVHqZx0eIt/X\n" \
"V8mb+DYloYzJNrrwpBhZLVxmkyBGoWs5y+dVnKp5Qdp218MzRVFgmYQhsMrd5B8c\n" \
"Fvov4Ghtz78HSS42wyJq+RGAYk7gKnJwW/N/KioxP3pswfCxKm/T6MNHkNm+8hH9\n" \
"N5mqnpDz/0twosPHXLvRdUFfbqAj8/wb5EXLtDvbGoq4YPlpqCCAvFFE/w9CWCRI\n" \
"URqbMDedAV+/SQJeAUXWsoZSaN7IEDjWSnFJODJWPLgBZ2ck7AUuSczeCuRudwZK\n" \
"h40UPG4BVcJ9NnEbcTAjfn1nheRPUZyJJufY2oi3QQKBgQC/gbpQ/S/TSlSHrWpf\n" \
"jJwv2JnC7p7zzLFeLKEwMTns1Et9puHGvmHZjl2f5iFOTtUMO5CyfIcQD7BoHXmA\n" \
"ydB8oY/GkTFY/MPv5oIvHkxp2fpoQ3/VUaZNa8ItuL7Ynk7SRNxrYgnYQfhs+1Qe\n" \
"YFwflgp/nxFgL6bzy6V/xeRM3wKBgQC2AyoEMX7C2TUzyLP9DYPxIvP9rhHq6qcg\n" \
"w0npZVYv30L//rgxuEt1vxoyU/V9lHo8NPgzQyIw1nOWmtnzglbAvospOS8dfJOY\n" \
"zSh0aAuQqmjVouABSbgTD940UATk2eYSM7YE5REowr1W8g9pzbqLTSgzhNsRmGTb\n" \
"bEC6CuoxiwKBgAx6OVd5h5oup59HbzzWfn6nkC0hOv4LgiruoXnwWyPRGTIQo3k9\n" \
"gkaYfgUjC1x8ymjHoL/gWTR0sTu3X+lCPPFB3YpEf4Cax3FkBWtof/YNm8EbVMLj\n" \
"VQCKWtZ7yQN4HQ6aKfpdIuMSOUtwfKSWBqqQLhLunEU2FZyg0iGnFR7lAoGAHPu8\n" \
"ARbwnI6CDlDzeGRikk+Cku7ZI5NiMbFnX6cnZlAjFyi0pBY4zfM0dLcx7GFsZZH5\n" \
"xA/4rdY4ac8WzdP/hInTFyln+0YPKtHgk0jOLqc0MnDRQuM+UKrCM4pafSbFoDhj\n" \
"z7u6U8dO5z18ftinz+7la+7IA7FEvjwqwnVifakCgYBeILoNY0/nv5kmcvR56dNA\n" \
"PfJ4RIGEF5ZD8V2wDAAoca4hKCdhAmxLKXqdVRO8lRVGwE32u4jIhgFwXKgNahiU\n" \
"suYQ0IMaT0UaW27+66I/ULDzMzW7aMzfuZfGX0RsFFHgDnofsS8xcz+6t3/oJ0yN\n" \
"wkkeZXawimEsUV9GNJYK6g==\n" \
"-----END PRIVATE KEY-----\n"

#define GOOD_NOBODY_CERT \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDwzCCAqugAwIBAgIUE5RHuxDAKfwMAr7F3b7hmr0yvSIwDQYJKoZIhvcNAQEN\n" \
"BQAwVzELMAkGA1UEBhMCVVMxHjAcBgNVBAoMFVRydXN0ZWQgVGVzdCBMb2NhbCBD\n" \
"QTEMMAoGA1UECwwDT3JnMRowGAYDVQQDDBF0cnVzdGVkLmxvY2FsaG9zdDAeFw0y\n" \
"MzA5MTYxNjI0NDNaFw0zMzA5MTMxNjI0NDNaMFIxCzAJBgNVBAYTAlVTMR0wGwYD\n" \
"VQQKDBRXcm9uZyBIb3N0IFRlc3QgQ2VydDEMMAoGA1UECwwDT3JnMRYwFAYDVQQD\n" \
"DA1ub3QubG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA\n" \
"iCiYaS6R0tofqJ33Mkp1L741Jf7XvJz4ab/6t+1W65uNEe5rTFy2v+HXxyfXcckG\n" \
"YNySa6wvwQf6k7sNZTc2f659MGATr2DjCxGA9jfdZPxxkbtzGG6aNViii7iNyxoN\n" \
"89Slyl0O+25ZLrlip2TeMH8f4Nog+h7VxjDX0tnh69VZp+821j2pNVtKNiZCceDj\n" \
"JXBVcvwTafR30kbILSdICukzgNK5eJ+uUmFLs9tdtCzJb4/EBh7tBz0MeCa60cwm\n" \
"LxCWbZdN6oZ0tDr8V5NlnBIieEaPZooCV4uItspj1D6IoG5ndbWg2iEmOqi7jQP2\n" \
"/XXTiNKjCmqs8bj2H4JsFQIDAQABo4GLMIGIMAkGA1UdEwQCMAAwCwYDVR0PBAQD\n" \
"AgWgMB0GA1UdJQQWMBQGCCsGAQUFBwMBBggrBgEFBQcDAjAPBgNVHREECDAGhwR/\n" \
"AAABMB0GA1UdDgQWBBRqNly3UzKEODdBBe3DWyV80TAb/zAfBgNVHSMEGDAWgBT7\n" \
"eDwBpUMrWJaGFUQYPSolxn83nzANBgkqhkiG9w0BAQ0FAAOCAQEAZDELdpm+qFnJ\n" \
"oXYapyCxAxPPvVXJJ+GUWls61wFz/EEza30U4FBvWa2v5gsXU1sj+axHSl8T3Eu6\n" \
"UNpRtb/YK5ktUcDSbftOFWl0Qyb4lKh2snbK9i8gKXrxk6c1eOiZUIuS+tiLnZd9\n" \
"bpOGIZgStRyOvfCq2inoAgNmuvL0Fql4FbFBR42GwFvQ4zvZh12V+IPaurVo7hAY\n" \
"9+v+E342vISUNEFi2GqcsoP+ALsPWEUIjodSesi7k+lRrLMQ4JAZUU110+j1h8Jw\n" \
"ZwPqi6fmQ4wMM5s12Fq5Zw0+/ldPUXr6+64bK4TnuJ1yh+2p+2YDX+14e5d9QTYW\n" \
"zk2gM2jXsw==\n" \
"-----END CERTIFICATE-----\n"


#define GOOD_EXPIRED_KEY \
"-----BEGIN PRIVATE KEY-----\n" \
"MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQCgPvzOGOp05Qb0\n" \
"XAWd75F/8VJlKfhFdjfJL6AC0IZ0IGl6W41uu7nE2ARVPXMFgpkdoTRpn1F5ReAz\n" \
"L09GL6EcMKxxBBIihA1BArmgqaiQK7e3e+QVsn5Nf5Z2USbsOmvcF6aGJnvWz1pV\n" \
"hPmLk7rYrtAvITP9pSWUZXpfnyWj+apiF+ZQ+H0rPW5P4hf/l9k4i+rQ0UpNpHJl\n" \
"jtVEGTLIDGTLmUJgO9uDo12jecxemb6fSGWC/IxSoNq9+McJqLdN5Ic52g5qmdfE\n" \
"cO+AMMcHkRrKS/9iY6By74DsGtCGa+I+u2Mg5MZJO7Ml/FTswW/hkl32fKRxqOHD\n" \
"IoXZR5+jAgMBAAECggEAQOZRv+zCZBZv0x7D3Zutu3oSq0sstIE1BcPGk+09TyHj\n" \
"Zj1XEh5wleMBGj65a2VhuujTj+WI+0tMCp7iBeR7ZS3nYRxLdfQyY/6FDKc50C4M\n" \
"iYDhNtJkKeH/H6stTuY4Udq4REOoyy6OgO1Knfd2aJSgVz4kztkIV8ojnj+X4Ikz\n" \
"nSoRo7owtXvhk/n9wb+LhvApvhOwf7wGgMrSReHCAQP0gGV3jVcNOsABFk7j76BR\n" \
"CT86maviUftRzKhgyMK5YJZ0A4K24vtEex7rtdtgnEobTsyIoSAQOei9Ppa5gwTG\n" \
"YJ/KCGKyy4sZ4ruZqvP0f2w9qxdJ9JWVnYcQcNop2QKBgQDid/YfXC5VyiGVVFi1\n" \
"7NtBJ740tRAT7/tGMa+9zca9QgjZ0OpkovvXvW+LFdX30X9M4Brm1somHYYbwsXg\n" \
"K84rB5zUzbukwkCSAFHLr4jGy9gExKD5XWMffxb80eW0yQMj/xXYoWmPtAW+wK5p\n" \
"BCOVQPzoGFqSm5jYUCBZg+FqmQKBgQC1JF27WIAYk9mLBi3cPPWN3wKHq7IuF10Z\n" \
"2aFr5YcRflY+NQo2roCCc878jIzgu4gNZU3MDgGMQMm2K7wxXgyGyxArlQBZvJ7u\n" \
"6PJk248ZGxyIo2l96y99hiZUZT7B0qd0kndDYmZQ1rrI+Js8bvlNfVu63TtQlRWi\n" \
"RFXFTrbdmwKBgQCx6EN1R1kvE+dmBCjcYgGsIaiOh17mCrR+5DGHDcx+iQ2i7zfz\n" \
"bwYg6TRjMvgwAmfa4ILBrSKI5tCfplcET+VacFSH6Ebsm51WFOIs/OyaHzDILh+3\n" \
"ReHEsHZCjjHB39eTw6RJ1iOzPFz3CS76WMILUk5bliYw3gMoi2meaLgAoQKBgQCN\n" \
"9+aOgppCqP4C++DNj+lUO8ib1HFdtmn9bJgbHFVz3HRzZNaQbMvDckhznyR6rf+/\n" \
"n/oNR5zm85Aj+rsQZgmjS1ttDOatbiuSS3hOm9XXc2w1mv1+9ujNlGpOEtvQxO0B\n" \
"bBmU8nYGPYU/Jblk8ATsOqe+GM34JiBCgRRIA51GqQKBgQDT5/2lYgvjSr+eiNgq\n" \
"EtfMhFgeXGvIJ2o1J+DSZmjned7Y4adfbJUoRqEFV8bjhCD7wRevVjC4YhKBwcea\n" \
"VoLz7wbmuZw+gm2nhGqRAsTQbknILmbQzBOT6a99C/zzbONt8fhFLbHmrnqIAi5v\n" \
"C65p2t0ubqelQkpt/1RiEiWB6Q==\n" \
"-----END PRIVATE KEY-----\n"

#define GOOD_EXPIRED_CERT \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDvDCCAqSgAwIBAgIUMO9RcsYD05iMzm5EFiwWr2DiFvAwDQYJKoZIhvcNAQEN\n" \
"BQAwVzELMAkGA1UEBhMCVVMxHjAcBgNVBAoMFVRydXN0ZWQgVGVzdCBMb2NhbCBD\n" \
"QTEMMAoGA1UECwwDT3JnMRowGAYDVQQDDBF0cnVzdGVkLmxvY2FsaG9zdDAeFw0y\n" \
"MzA5MTYxNjI0NDNaFw0yMzA5MTUxNjI0NDNaMEsxCzAJBgNVBAYTAlVTMRowGAYD\n" \
"VQQKDBFFeHBpcmVkIFRlc3QgQ2VydDEMMAoGA1UECwwDT3JnMRIwEAYDVQQDDAkx\n" \
"MjcuMC4wLjEwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCgPvzOGOp0\n" \
"5Qb0XAWd75F/8VJlKfhFdjfJL6AC0IZ0IGl6W41uu7nE2ARVPXMFgpkdoTRpn1F5\n" \
"ReAzL09GL6EcMKxxBBIihA1BArmgqaiQK7e3e+QVsn5Nf5Z2USbsOmvcF6aGJnvW\n" \
"z1pVhPmLk7rYrtAvITP9pSWUZXpfnyWj+apiF+ZQ+H0rPW5P4hf/l9k4i+rQ0UpN\n" \
"pHJljtVEGTLIDGTLmUJgO9uDo12jecxemb6fSGWC/IxSoNq9+McJqLdN5Ic52g5q\n" \
"mdfEcO+AMMcHkRrKS/9iY6By74DsGtCGa+I+u2Mg5MZJO7Ml/FTswW/hkl32fKRx\n" \
"qOHDIoXZR5+jAgMBAAGjgYswgYgwCQYDVR0TBAIwADALBgNVHQ8EBAMCBaAwHQYD\n" \
"VR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMA8GA1UdEQQIMAaHBH8AAAEwHQYD\n" \
"VR0OBBYEFC12L2ARelusQlbrfL18bKxG3xjvMB8GA1UdIwQYMBaAFPt4PAGlQytY\n" \
"loYVRBg9KiXGfzefMA0GCSqGSIb3DQEBDQUAA4IBAQCexh/TY/S7Vh5/7UjHdHdd\n" \
"VmtX1BYgGV/i188vXoBQCiYiV4b/9gpeZXYxv3qbbW9QGH/agjtvsBqDPU4wgCm2\n" \
"qK58FnLWjNr3SfsViWIMcfN1/7ltGx18mgeo8PTqAA5uFbWqd1yPXE9qQPM173UR\n" \
"LpjuWC/SP+z9OwZ0Usm+8/7xhVJ7QSr4/UMQhPvTjUsAZPXr8K0V02Wmiu7ug16O\n" \
"8dDJn9nqlhqUcL1g8ssYoiVtDRhFnvxQiKGther+b92sDy9RV8PlyxK645m/bOmQ\n" \
"l28+aImMa8YFB9fjccY3a/yJhyPLm0/17BN7qArtjLpbXxRe9YKqfVS95M1KcHcS\n" \
"-----END CERTIFICATE-----\n"

#define BAD_CA_KEY \
"-----BEGIN PRIVATE KEY-----\n" \
"MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDX6f+LBQkq5XnO\n" \
"45Y1DFfnmkgCtC6i3KSJE33flbPtE6qeKMYwSl+wAWoM5NytGgQb+invlP59Dr09\n" \
"ZiEhSqLiQjmYaI6s5xyV7AeliuTW7VefyX6XC6Cy7gvAHWOKWq8jjvHwtVOErXqe\n" \
"b8zP2mcKBQUu3fKhnBi2/cWaMgyvK7YwjTfH4yJAOc96M980DZIsvEY/caX4qZ+h\n" \
"Gkndgbg1RIhcRcpUXZaGF9ZYi/3biWVEHignzrpfd9Vx7p+BCKG5CrhEKqLuo0ws\n" \
"+gD1xALDMMXI7VNGBPFVovx5mH9oCsWsI8V7LJxrc7Kt3gSll8+U9f1pcC8+0lWH\n" \
"6Kow5AE7AgMBAAECggEAY2BcMcw7j/bWvZaaiAgKXZrxEe2EmYkcYcsK4GGy0qd+\n" \
"nBUAQzG4IPZFrXGY8ENNKONXceINz9l82EmtbflARwtcHv3gzUwCKC7exh7LfPD4\n" \
"p57CGRtM944A8oeUTRW4DpG4YaXTrvl5unqStgg77Kyq2gtlpWSHK5JZ5MKse+K1\n" \
"9cRKA/VITlaIElmbQm63KKD5aJufA1vuS4bVwUL/D5WTGBg5oHaMsFBnq9Sn1HWy\n" \
"4kRclx4n9Bf+ONyiIaKf9LmSwwqMblpNQk9EDOGiQFf4A02wFQXw0LrebdiPF4T+\n" \
"kHB4fpvubEU/dxAKhqkBGSprSSjhCE+v+2Rk/w06IQKBgQD1ZAUt8yQYbIJJBzpS\n" \
"Mznk5jUD37MbgLgQ4/LV62P6R2wTyj5387RUqVIcQKJbgB337nnklVUyv8EUAH6x\n" \
"1b7wkcPiVy6mrXd3exDuWqACZf9Dr2/CAIHYdv7Xw9vmLdHEYto/vmnrUqaXNt2E\n" \
"zVrZgy+oqYMMPYMj1WWtltr83QKBgQDhP7shsbfW1zkvZkxNP7/NzZJGZn2847yV\n" \
"xUq/aSohTmM2BkV/3JpYLPtL7lLouKlAY1Q5j98TTL78O73Y1wGSz0VqCNgkGgGa\n" \
"ZseUhddSIJycPtpAAvUT/hRZDSB3AecSSCzUIWl35KkF58fTvZfXZW0UHeRtcUNP\n" \
"V205ET2o9wKBgDIWbVw4sdhuZZWEbSoUHLAVMSMOSR/HAUspTArQFkMiyvOrJ18S\n" \
"lm9ldEYiB0HH/9X4jlbTCGXob/+mLjYcW/H8vs/3XaVV6PmxW/5a7yguK5FkQNqH\n" \
"gfluKIxgBQWRuqxsRQIX2sTWxPw+ja6qv+1/8n1pxD7+W0M4Lp3lGePdAoGAYTN4\n" \
"VHT1iT99DOhPOvKsmWoBmmG2FuILynHF7M28AX1rMLAKI8AV1sEqfDzPCGBPoVp7\n" \
"yQ89y31N2VkKdt1pb2oEYVqJsyKJ2JBLxdrv7R/nlUFGO2NLUSJOs5MHlHN8vJXq\n" \
"ymvUsIk7KzI3ODdTo/6/0HOJ1HSRAFcQ6hR3kEcCgYA0FufRy7N0KpHcej9FxD8w\n" \
"MAc0DYNuModZIB5CcYM04ELLIEWBuGluz6udfEQtUnV1OGsoQBe98L4WLuUQtjRH\n" \
"UlaOCYEoLjq+lWJBwxL1ORD7AcePNxsDHi/b358oRKaTMFf7PBSnm83hFSnABRzM\n" \
"lnBqDWgNklkfwIWKrsioiA==\n" \
"-----END PRIVATE KEY-----\n"

#define BAD_CA_CERT \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDpzCCAo+gAwIBAgIUaR5BE9UBNAnF5XPR4r3kPUCU3Q8wDQYJKoZIhvcNAQEN\n" \
"BQAwWzELMAkGA1UEBhMCVVMxIDAeBgNVBAoMF1VudHJ1c3RlZCBUZXN0IExvY2Fs\n" \
"IENBMQwwCgYDVQQLDANPcmcxHDAaBgNVBAMME3VudHJ1c3RlZC5sb2NhbGhvc3Qw\n" \
"HhcNMjMwOTE2MTYyNDQyWhcNMzMwOTEzMTYyNDQyWjBbMQswCQYDVQQGEwJVUzEg\n" \
"MB4GA1UECgwXVW50cnVzdGVkIFRlc3QgTG9jYWwgQ0ExDDAKBgNVBAsMA09yZzEc\n" \
"MBoGA1UEAwwTdW50cnVzdGVkLmxvY2FsaG9zdDCCASIwDQYJKoZIhvcNAQEBBQAD\n" \
"ggEPADCCAQoCggEBANfp/4sFCSrlec7jljUMV+eaSAK0LqLcpIkTfd+Vs+0Tqp4o\n" \
"xjBKX7ABagzk3K0aBBv6Ke+U/n0OvT1mISFKouJCOZhojqznHJXsB6WK5NbtV5/J\n" \
"fpcLoLLuC8AdY4paryOO8fC1U4Step5vzM/aZwoFBS7d8qGcGLb9xZoyDK8rtjCN\n" \
"N8fjIkA5z3oz3zQNkiy8Rj9xpfipn6EaSd2BuDVEiFxFylRdloYX1liL/duJZUQe\n" \
"KCfOul931XHun4EIobkKuEQqou6jTCz6APXEAsMwxcjtU0YE8VWi/HmYf2gKxawj\n" \
"xXssnGtzsq3eBKWXz5T1/WlwLz7SVYfoqjDkATsCAwEAAaNjMGEwHQYDVR0OBBYE\n" \
"FNHltVZoVejO07NttPIIcGfL2pBMMB8GA1UdIwQYMBaAFNHltVZoVejO07NttPII\n" \
"cGfL2pBMMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgGGMA0GCSqGSIb3\n" \
"DQEBDQUAA4IBAQCGYyfDGut7VSdrlG1j+Rgu6hz+xwgPVBq/aC/ojnesrr8lpOWU\n" \
"gN4almC1mV0d+a4De6XkQ2ZfVlBfCTP+9L6tKwrbQdUreCukLGJnx7RyTy1SgmMb\n" \
"CIUKw7FAZr3gXJx5MRBDmKcgTtO1oCivmRO3WFuDccrJc3ASy4ZWjdphgvS7jt8u\n" \
"5W/mIEPDxVpWlf4fYG0BSIf3z/qv1+rnrtErxERLfCK+IQss39k8lZGgPcpZh54g\n" \
"eZZzRb2VX1OBwLxp/5RF7il2aPJfn4Zl8hhqaTlbger/gJGhC8qNv4mhYeyOLYPR\n" \
"ALGtfOFyj9porZY8RohPk7a1Whow2+uJAEcn\n" \
"-----END CERTIFICATE-----\n"

#define BAD_127_0_0_1_KEY \
"-----BEGIN PRIVATE KEY-----\n" \
"MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQC/up64TdZNnfEE\n" \
"GkHDs8LtwYKKpaaTp5eLWfDWUiLz2WdO/KjF5L9yrip1dqy56EoBepE+eazUEDyc\n" \
"50HmFkXhyksFNC/dUTwe+FyH7dX5XgP74haC8/Rjh0VdynaZwmepPv0UQjpOvOa1\n" \
"C23ARuy0FS3NofpWiyksJUx6bLgXI4VYQnV3b3G7MkKy1SggJZ9VIWMG4Fl3rm3f\n" \
"4bH2PWAYcA9Ib4PGI9qtVxa0dvXpjLtfcWf0UmZOnMIrl+Lzymdl0Di2K24u3Qlm\n" \
"/g9atyUcTxVEzRfU31EBd26KZHyHGP7UVp9lNx3jLsr6sVt+AH5maFMiY/+H7xpL\n" \
"jMGcz6OjAgMBAAECggEAB8UoaY+9/6dG/UOTJrtCSyBsifHsrk88dQlQGV96yyck\n" \
"aK8Up/B9Uch9MAV/OomPdj/hYwbLWkzOKA23PfvpGR15rNxQlEeWiCrIlTmuUkzd\n" \
"HHgtMWFxphb1WLe71r9qNBG5b/i/JDHDp9Kuv/wf26yADrVhz+AT6xYX415El6tm\n" \
"vqWEjdrq80RzBhVdoQsI4/IHLa8w+K2yzoH/8KffhwCruykWLnFWTTflbrxpe7B7\n" \
"FupkmDxaa0KhF9xD5ujBWREqcMNFA6QveyvU2L6nj8ltmWKWplmFJZH9eFiEijzW\n" \
"5iEQ6asBxhtmHf+RMEVnAJYkHF43IbozcVCO5N6gAQKBgQD85eSwPfrH/1n2/tWL\n" \
"E6C4zJr5pu6HZrme0+o9mRMN9DynfjW6um0dV3DBqA6fBWcTOFORa0kmAKjanEW8\n" \
"Es+OL9pf5a4+VGGNA+/tgiBu/YBZxKQfPVwnuCLKNAw08+/YIGNMjeqiwBrvdtH5\n" \
"DU0HAoxmGVPGIAc2ChkNG3X/gQKBgQDCFKd8atsjvXHn/TbGEdpN9Pd1vWcnq00l\n" \
"xhsS84t1uDraT+4W+HuuDZdtFXuYbHCR2XrNB2cRTjy86QVzrmkWuM1pl6ZExxbx\n" \
"p1zH9Y1tyeSTIhHVzXjM2BB+VDm8g5spgEqMaKfohgyCtKySQ1fHAW+Fd+AUZjw2\n" \
"bD9AUcg1IwKBgQDhdC1otQhx7YINgOdsggYBWV47fAcfrIOERZWIboOfKAI+q3RI\n" \
"0FPgGYjLDABk4dMfPlK5zDQ87c+afEBqxzZDowOCBGdO57jDlyP4z84MRHB9FqHd\n" \
"u70BJ9XT/4x2VJWYTl9X6xinGK2ql3nfFm1591qk1qVpdjoXge8NUVLjgQKBgQCB\n" \
"redTXNrcAKNxjWHh6t/hIwOOKmYYvX8o9Dv258CRge3hHXNK6fFKFU7O1oHlEOAO\n" \
"tDA3evDFQW+YEmNQLoONaSHnoiq43gZYpal6+nnrl/Eg1qzwHQDQbrZmksSZT56H\n" \
"lm85blxzI86ML0j32gN2t2Da34RcXQtymdWRlpOd3QKBgQCiLfAoFDZRAF54pcJg\n" \
"xKeaWoJwldEFSafB1E7OsC6oigBHlhGGUU+8x498E0AfOqNSdXBIXXKEYDHqZ0Jn\n" \
"1+FbEIof8H96TreMKiB9ZIWSX9P6p0k0hgOmJDcvjBmS8IDRdtCh+CBBUJaqHc1c\n" \
"cOVeZKhilAX+IfiNjEUic9uFBA==\n" \
"-----END PRIVATE KEY-----\n"

#define BAD_127_0_0_1_CERT \
"-----BEGIN CERTIFICATE-----\n" \
"MIIDwjCCAqqgAwIBAgIUFTrbIP/QPmFBJCX1pJCo7I1+/bswDQYJKoZIhvcNAQEN\n" \
"BQAwWzELMAkGA1UEBhMCVVMxIDAeBgNVBAoMF1VudHJ1c3RlZCBUZXN0IExvY2Fs\n" \
"IENBMQwwCgYDVQQLDANPcmcxHDAaBgNVBAMME3VudHJ1c3RlZC5sb2NhbGhvc3Qw\n" \
"HhcNMjMwOTE2MTYyNDQzWhcNMzMwOTEzMTYyNDQzWjBNMQswCQYDVQQGEwJVUzEc\n" \
"MBoGA1UECgwTSm9lTm9ib2R5IFRlc3QgQ2VydDEMMAoGA1UECwwDT3JnMRIwEAYD\n" \
"VQQDDAkxMjcuMC4wLjEwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC/\n" \
"up64TdZNnfEEGkHDs8LtwYKKpaaTp5eLWfDWUiLz2WdO/KjF5L9yrip1dqy56EoB\n" \
"epE+eazUEDyc50HmFkXhyksFNC/dUTwe+FyH7dX5XgP74haC8/Rjh0VdynaZwmep\n" \
"Pv0UQjpOvOa1C23ARuy0FS3NofpWiyksJUx6bLgXI4VYQnV3b3G7MkKy1SggJZ9V\n" \
"IWMG4Fl3rm3f4bH2PWAYcA9Ib4PGI9qtVxa0dvXpjLtfcWf0UmZOnMIrl+Lzymdl\n" \
"0Di2K24u3Qlm/g9atyUcTxVEzRfU31EBd26KZHyHGP7UVp9lNx3jLsr6sVt+AH5m\n" \
"aFMiY/+H7xpLjMGcz6OjAgMBAAGjgYswgYgwCQYDVR0TBAIwADALBgNVHQ8EBAMC\n" \
"BaAwHQYDVR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMA8GA1UdEQQIMAaHBH8A\n" \
"AAEwHQYDVR0OBBYEFJrM/krza0YF6WQnKkd4BltLN2Y5MB8GA1UdIwQYMBaAFNHl\n" \
"tVZoVejO07NttPIIcGfL2pBMMA0GCSqGSIb3DQEBDQUAA4IBAQBCzrKOPKPjbVBv\n" \
"lPCO1rflVFD517J079rCXzxOFgENCBPCMT1MeJ3fjUs/Xxf9S2OhB4bHregEmG46\n" \
"52uKDHEzRodEak8gomYfU2NmKS95Jy7uw8qavxW7EMroOEBgDqY09wG/pBBZ0tdj\n" \
"Mm4oVYmgt8EWwfS98ejRxgFZ7dASh7fMtqQoS79r6i9hl+besFHqN5SRidMg0WGN\n" \
"pT2zMxquOCRXhf65BPcJBf4887j0UedGbCRLhSH+7Ix4wM9iOibHP3cGJqGdyEiz\n" \
"05tgR1nk+8tye1maxNBaDpIqx5d9VuYBLvPNqHyVvcHq0wFXLrGrB0nbmgh9XT0K\n" \
"yrtcysM8\n" \
"-----END CERTIFICATE-----\n"

static char _ca_good_key[] = GOOD_CA_KEY;
static char _ca_good_cert[] = GOOD_CA_CERT;

static char _good_127_0_0_1_key[] = GOOD_127_0_0_1_KEY;
static char _good_127_0_0_1_cert[] = GOOD_127_0_0_1_CERT;
static char _good_127_0_0_1_chain[] = GOOD_127_0_0_1_CERT GOOD_CA_CERT;

static char _good_nobody_key[] = GOOD_NOBODY_KEY;
static char _good_nobody_cert[] = GOOD_NOBODY_CERT;
static char _good_nobody_chain[] = GOOD_NOBODY_CERT GOOD_CA_CERT;

static char _good_expired_key[] = GOOD_EXPIRED_KEY;
static char _good_expired_cert[] = GOOD_EXPIRED_CERT;
static char _good_expired_chain[] = GOOD_EXPIRED_CERT GOOD_CA_CERT;

static char _ca_bad_key[] = BAD_CA_KEY;
static char _ca_bad_cert[] = BAD_CA_CERT;

static char _bad_127_0_0_1_key[] = BAD_127_0_0_1_KEY;
static char _bad_127_0_0_1_cert[] = BAD_127_0_0_1_CERT;
static char _bad_127_0_0_1_chain[] = BAD_127_0_0_1_CERT BAD_CA_CERT;

#define MKDSTR(arr) {arr, sizeof(arr), sizeof(arr)-1, true}

dstr_t ca_good_key = MKDSTR(_ca_good_key);
dstr_t ca_good_cert = MKDSTR(_ca_good_cert);

dstr_t good_127_0_0_1_key = MKDSTR(_good_127_0_0_1_key);
dstr_t good_127_0_0_1_cert = MKDSTR(_good_127_0_0_1_cert);
dstr_t good_127_0_0_1_chain = MKDSTR(_good_127_0_0_1_chain);

dstr_t good_nobody_key = MKDSTR(_good_nobody_key);
dstr_t good_nobody_cert = MKDSTR(_good_nobody_cert);
dstr_t good_nobody_chain = MKDSTR(_good_nobody_chain);

dstr_t good_expired_key = MKDSTR(_good_expired_key);
dstr_t good_expired_cert = MKDSTR(_good_expired_cert);
dstr_t good_expired_chain = MKDSTR(_good_expired_chain);

dstr_t ca_bad_cert = MKDSTR(_ca_bad_cert);
dstr_t ca_bad_key = MKDSTR(_ca_bad_key);

dstr_t bad_127_0_0_1_key = MKDSTR(_bad_127_0_0_1_key);
dstr_t bad_127_0_0_1_cert = MKDSTR(_bad_127_0_0_1_cert);
dstr_t bad_127_0_0_1_chain = MKDSTR(_bad_127_0_0_1_chain);

derr_t trust_ca(SSL_CTX *ctx, dstr_t ca_cert){
    derr_t e = E_OK;

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    if(!store) ORIG(&e, E_SSL, "unable to get store: %x\n", FSSL);

    // wrap ca bytes in a mem bio
    BIO* pembio = BIO_new_mem_buf((void*)ca_cert.data, (int)ca_cert.len);
    if(!pembio){
        ORIG(&e, E_NOMEM, "unable to create BIO: %x", FSSL);
    }

    // read the public key from the BIO (no password protection)
    X509 *x509 = PEM_read_bio_X509(pembio, NULL, NULL, NULL);
    BIO_free(pembio);
    if(!x509){
        ORIG(&e, E_SSL, "unable to read CA: %x", FSSL);
    }

    int ret = X509_STORE_add_cert(store, x509);
    X509_free(x509);
    if(ret != 1){
        ORIG(&e, E_SSL, "unable to add cert to store cert: %x\n", FSSL);
    }

    return e;
}

derr_t trust_good(SSL_CTX *ctx){
    return trust_ca(ctx, ca_good_cert);
}

derr_t trust_bad(SSL_CTX *ctx){
    return trust_ca(ctx, ca_bad_cert);
}

#define MKCTX(name) \
    derr_t e = E_OK; \
    *out = NULL; \
    ssl_context_t ctx; \
    PROP(&e, ssl_context_new_server_pem(&ctx, name##_chain, name##_key) ); \
    *out = ctx.ctx; \
    return e

derr_t good_127_0_0_1_server(SSL_CTX **out){
    MKCTX(good_127_0_0_1);
}

derr_t good_nobody_server(SSL_CTX **out){
    MKCTX(good_nobody);
}

derr_t good_expired_server(SSL_CTX **out){
    MKCTX(good_expired);
}

derr_t bad_127_0_0_1_server(SSL_CTX **out){
    MKCTX(bad_127_0_0_1);
}

static char _ca_pebble_cert[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDCTCCAfGgAwIBAgIIJOLbes8sTr4wDQYJKoZIhvcNAQELBQAwIDEeMBwGA1UE\n"
"AxMVbWluaWNhIHJvb3QgY2EgMjRlMmRiMCAXDTE3MTIwNjE5NDIxMFoYDzIxMTcx\n"
"MjA2MTk0MjEwWjAgMR4wHAYDVQQDExVtaW5pY2Egcm9vdCBjYSAyNGUyZGIwggEi\n"
"MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC5WgZNoVJandj43kkLyU50vzCZ\n"
"alozvdRo3OFiKoDtmqKPNWRNO2hC9AUNxTDJco51Yc42u/WV3fPbbhSznTiOOVtn\n"
"Ajm6iq4I5nZYltGGZetGDOQWr78y2gWY+SG078MuOO2hyDIiKtVc3xiXYA+8Hluu\n"
"9F8KbqSS1h55yxZ9b87eKR+B0zu2ahzBCIHKmKWgc6N13l7aDxxY3D6uq8gtJRU0\n"
"toumyLbdzGcupVvjbjDP11nl07RESDWBLG1/g3ktJvqIa4BWgU2HMh4rND6y8OD3\n"
"Hy3H8MY6CElL+MOCbFJjWqhtOxeFyZZV9q3kYnk9CAuQJKMEGuN4GU6tzhW1AgMB\n"
"AAGjRTBDMA4GA1UdDwEB/wQEAwIChDAdBgNVHSUEFjAUBggrBgEFBQcDAQYIKwYB\n"
"BQUHAwIwEgYDVR0TAQH/BAgwBgEB/wIBADANBgkqhkiG9w0BAQsFAAOCAQEAF85v\n"
"d40HK1ouDAtWeO1PbnWfGEmC5Xa478s9ddOd9Clvp2McYzNlAFfM7kdcj6xeiNhF\n"
"WPIfaGAi/QdURSL/6C1KsVDqlFBlTs9zYfh2g0UXGvJtj1maeih7zxFLvet+fqll\n"
"xseM4P9EVJaQxwuK/F78YBt0tCNfivC6JNZMgxKF59h0FBpH70ytUSHXdz7FKwix\n"
"Mfn3qEb9BXSk0Q3prNV5sOV3vgjEtB4THfDxSz9z3+DepVnW3vbbqwEbkXdk3j82\n"
"2muVldgOUgTwK8eT+XdofVdntzU/kzygSAtAQwLJfn51fS1GvEcYGBc1bDryIqmF\n"
"p9BI7gVKtWSZYegicA==\n"
"-----END CERTIFICATE-----\n";

dstr_t ca_pebble_cert = MKDSTR(_ca_pebble_cert);

derr_t trust_pebble(SSL_CTX *ctx){
    return trust_ca(ctx, ca_pebble_cert);
}
