/* Host tests for awdl_census.h -- the SAME code the firmware compiles.
 *
 * The mDNS fixtures are REAL bytes captured off the air, not invented ones. That matters: the whole reason the census
 * exists is that AirDrop's instance label turned out to be an opaque hex id rather
 * than a device name, and only real payloads could have shown that.
 *
 *   cc -O2 -o /tmp/census_test tools/census_test.c && /tmp/census_test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../ESP32Drop/src/awdl/core/awdl_census.h"

static int fails = 0, ran = 0;
static void check(const char *name, int ok, const char *detail) {
  ran++;
  printf("  [%s] %-56s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* hex string -> bytes */
static int unhex(const char *h, unsigned char *out, int max) {
  int n = 0;
  for (const char *p = h; p[0] && p[1] && n < max; p += 2) {
    char b[3] = { p[0], p[1], 0 };
    out[n++] = (unsigned char)strtol(b, 0, 16);
  }
  return n;
}

static const char *peer_names(struct Census *c, const unsigned char *mac) {
  static char buf[256];
  buf[0] = 0;
  for (int i = 0; i < CEN_PEERS; i++) {
    if (!c->peer[i].used || memcmp(c->peer[i].mac, mac, 6)) continue;
    for (int k = 0; k < c->peer[i].n_names; k++) {
      strcat(buf, k ? "|" : "");
      strcat(buf, c->peer[i].name[k].kind == 1 ? "host:" : "svc:");
      strcat(buf, c->peer[i].name[k].s);
    }
  }
  return buf;
}

int main(void) {
  printf("== awdl_census.h host tests ==\n");
  static unsigned char b[1024];
  const unsigned char MAC_A[6] = {0xe2,0xa6,0xfb,0xcd,0xa8,0xa4};
  const unsigned char MAC_B[6] = {0xce,0x57,0xf1,0xaf,0x9d,0x17};

  /* 1. REAL capture: an AirDrop query. Its instance label "fdba584fc267" is opaque
     hex -- a session id, not a device. Remembering it would split one Mac into many
     identities over time, so it must be dropped. */
  {
    int n = unhex("0000000000010000000000000c666462613538346663323637085f61697264726f70"
                  "045f746370056c6f63616c000010000100007856", b, sizeof b);
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_A, b, n, 1000);
    const char *nm = peer_names(&c, MAC_A);
    check("real AirDrop query: opaque hex instance is NOT kept as a name",
          nm[0] == 0, nm[0] ? nm : "(no names, correct)");
  }

  /* 2. REAL capture: the longer AirDrop response, same conclusion. */
  {
    int n = unhex("0000000000040000000000000c326138343835343362333063085f61697264726f70"
                  "045f746370056c6f63616c00001000010c666462613538346663323637c0190010"
                  "0001c00c00210001c00c0010000100000078", b, sizeof b);
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_A, b, n, 1000);
    const char *nm = peer_names(&c, MAC_A);
    check("real AirDrop response: still no name (all instances are hex)",
          nm[0] == 0, nm[0] ? nm : "(no names, correct)");
  }

  /* 3. A hostname record: "Miros-MacBook-Pro.local" -- THIS is a device identity,
     and it is what survives a MAC rotation. */
  {
    /* header(12) + name: 17 "Miros-MacBook-Pro" 5 "local" 0 + type AAAA(28) class IN */
    int n = 0;
    memset(b, 0, 12); b[2] = 0x84; n = 12;   /* QR=1: an announcement */
    b[n++] = 17; memcpy(b + n, "Miros-MacBook-Pro", 17); n += 17;
    b[n++] = 5;  memcpy(b + n, "local", 5); n += 5;
    b[n++] = 0;
    b[n++] = 0; b[n++] = 28; b[n++] = 0; b[n++] = 1;
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_A, b, n, 1000);
    check("hostname <device>.local is captured as a device name",
          strcmp(peer_names(&c, MAC_A), "host:Miros-MacBook-Pro") == 0, peer_names(&c, MAC_A));
  }

  /* 4. A service instance that carries a real name (_companion-link._tcp is how
     Apple devices announce themselves to each other). */
  {
    int n = 12; memset(b, 0, 12); b[2] = 0x84;   /* QR=1 */
    b[n++] = 10; memcpy(b + n, "Demo-iPad2", 10); n += 10;
    b[n++] = 15; memcpy(b + n, "_companion-link", 15); n += 15;
    b[n++] = 4; memcpy(b + n, "_tcp", 4); n += 4;
    b[n++] = 5; memcpy(b + n, "local", 5); n += 5;
    b[n++] = 0;
    b[n++] = 0; b[n++] = 12; b[n++] = 0; b[n++] = 1;
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_A, b, n, 1000);
    check("service instance name is captured (@ = service kind)",
          strstr(peer_names(&c, MAC_A), "svc:Demo-iPad2") != 0, peer_names(&c, MAC_A));
  }

  /* 5. Names must attach to the RIGHT mac, and two peers stay separate. */
  {
    int n = 12; memset(b, 0, 12); b[2] = 0x84;   /* QR=1 */
    b[n++] = 7; memcpy(b + n, "Kuro-Ai", 7); n += 7;
    b[n++] = 5; memcpy(b + n, "local", 5); n += 5;
    b[n++] = 0; b[n++] = 0; b[n++] = 28; b[n++] = 0; b[n++] = 1;
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_B, b, n, 2000);
    int a_empty = peer_names(&c, MAC_A)[0] == 0;
    int b_named = strcmp(peer_names(&c, MAC_B), "host:Kuro-Ai") == 0;
    check("a name binds only to the mac that sent it", a_empty && b_named,
          peer_names(&c, MAC_B));
  }

  /* 6. RSSI aggregation: last / average / min..max, which is the proximity handle
     used to identify a device by MOVING it rather than toggling its Wi-Fi. */
  {
    struct Census c; cen_init(&c);
    cen_frame(&c, MAC_A, -40, 100);
    cen_frame(&c, MAC_A, -60, 200);
    cen_frame(&c, MAC_A, -50, 300);
    char line[256];
    for (int i = 0; i < CEN_PEERS; i++)
      if (c.peer[i].used) cen_format(&c.peer[i], 500, line, sizeof line);
    int ok = strstr(line, "rssi=-50/-50[-60..-40]") && strstr(line, "frames=3")
             && strstr(line, "age=200");
    check("RSSI last/avg/min/max and age are reported", ok != 0, line);
  }

  /* 7. Garbage must not produce names (the scan runs on hostile input). */
  {
    struct Census c; cen_init(&c);
    for (int i = 0; i < 200; i++) b[i] = (unsigned char)(i * 7 + 3);
    cen_mdns(&c, MAC_A, b, 200, 1000);
    const char *nm = peer_names(&c, MAC_A);
    check("random bytes yield no names", nm[0] == 0, nm[0] ? nm : "(none)");
  }

  /* 8. A compression pointer must not send the walker into a loop. */
  {
    int n = 12; memset(b, 0, 12);
    b[n++] = 4; memcpy(b + n, "Test", 4); n += 4;
    b[n++] = 0xc0; b[n++] = 0x0c;            /* pointer back into the header */
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_A, b, n, 1000);         /* must simply return */
    check("compression pointer terminates the walk (no hang)", 1, "returned");
  }

  /* 9. Duplicate sightings of the same name are stored once. */
  {
    int n = 12; memset(b, 0, 12); b[2] = 0x84;   /* QR=1 */
    b[n++] = 7; memcpy(b + n, "Kuro-Ai", 7); n += 7;
    b[n++] = 5; memcpy(b + n, "local", 5); n += 5;
    b[n++] = 0; b[n++] = 0; b[n++] = 28; b[n++] = 0; b[n++] = 1;
    struct Census c; cen_init(&c);
    for (int i = 0; i < 5; i++) cen_mdns(&c, MAC_A, b, n, 1000 + i);
    int cnt = 0;
    for (int i = 0; i < CEN_PEERS; i++)
      if (c.peer[i].used && !memcmp(c.peer[i].mac, MAC_A, 6)) cnt = c.peer[i].n_names;
    check("repeated names are de-duplicated", cnt == 1, cnt == 1 ? "1 name" : "duplicated");
  }

  /* 10. THE ATTRIBUTION BUG, from the census's own first real capture. A QUERY names
     what the sender is asking ABOUT, not the sender. Our badge's own "M5 Badge"
     service turned up attached to a neighbour that had merely queried for it. Only
     responses may name their sender. */
  {
    int n = 12; memset(b, 0, 12);
    /* header stays all-zero => QR=0 => this is a QUERY */
    b[n++] = 9; memcpy(b + n, "M5 Badge2", 9); n += 9;
    b[n++] = 5; memcpy(b + n, "local", 5); n += 5;
    b[n++] = 0; b[n++] = 0; b[n++] = 28; b[n++] = 0; b[n++] = 1;
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_B, b, n, 1000);
    const char *nm = peer_names(&c, MAC_B);
    check("a QUERY does not name its sender (it names somebody else)",
          nm[0] == 0, nm[0] ? nm : "(no names, correct)");
  }

  /* 11. The same bytes as a RESPONSE do name the sender. */
  {
    int n = 12; memset(b, 0, 12);
    b[2] = 0x84;                                   /* QR=1, authoritative */
    b[n++] = 9; memcpy(b + n, "M5 Badge2", 9); n += 9;
    b[n++] = 5; memcpy(b + n, "local", 5); n += 5;
    b[n++] = 0; b[n++] = 0; b[n++] = 28; b[n++] = 0; b[n++] = 1;
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_B, b, n, 1000);
    check("a RESPONSE does name its sender",
          strcmp(peer_names(&c, MAC_B), "host:M5 Badge2") == 0, peer_names(&c, MAC_B));
  }

  /* 12. Opaque ids seen in the first real capture must all be rejected: a UUID, a
     TXT key=value fragment, and a name with a long hex tail. */
  {
    const char *junk[] = { "22b839e1-686e-437b-b1db-83d554ef9a83",
                           "sn=com", "CLink-bde27f44a935" };
    int rejected = 0;
    for (int j = 0; j < 3; j++) {
      int L = (int)strlen(junk[j]);
      int n = 12; memset(b, 0, 12); b[2] = 0x84;   /* response */
      b[n++] = (unsigned char)L; memcpy(b + n, junk[j], L); n += L;
      b[n++] = 5; memcpy(b + n, "local", 5); n += 5;
      b[n++] = 0; b[n++] = 0; b[n++] = 28; b[n++] = 0; b[n++] = 1;
      struct Census c; cen_init(&c);
      cen_mdns(&c, MAC_A, b, n, 1000);
      if (peer_names(&c, MAC_A)[0] == 0) rejected++;
    }
    char d[80]; snprintf(d, sizeof d, "%d/3 rejected", rejected);
    check("UUIDs, TXT fragments and hex-tailed ids are not device names",
          rejected == 3, d);
  }

  /* 13. ...but a normal device name with a digit still survives the filters. */
  {
    int n = 12; memset(b, 0, 12); b[2] = 0x84;
    b[n++] = 11; memcpy(b + n, "Demo-iPhone", 11); n += 11;
    b[n++] = 5; memcpy(b + n, "local", 5); n += 5;
    b[n++] = 0; b[n++] = 0; b[n++] = 28; b[n++] = 0; b[n++] = 1;
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_A, b, n, 1000);
    check("a real device name is still kept (filters are not too greedy)",
          strcmp(peer_names(&c, MAC_A), "host:Demo-iPhone") == 0, peer_names(&c, MAC_A));
  }

  /* 14. Service TYPES identify what KIND of device a silent peer is. Some
     neighbours offer no hostname, so the type is the only clue. */
  {
    int n = 12; memset(b, 0, 12); b[2] = 0x84;
    b[n++] = 8; memcpy(b + n, "_airplay", 8); n += 8;
    b[n++] = 4; memcpy(b + n, "_tcp", 4); n += 4;
    b[n++] = 5; memcpy(b + n, "local", 5); n += 5;
    b[n++] = 0; b[n++] = 0; b[n++] = 12; b[n++] = 0; b[n++] = 1;
    struct Census c; cen_init(&c);
    cen_mdns(&c, MAC_A, b, n, 1000);
    int ok = 0;
    for (int i = 0; i < CEN_PEERS; i++)
      if (c.peer[i].used && !memcmp(c.peer[i].mac, MAC_A, 6))
        for (int k = 0; k < c.peer[i].n_names; k++)
          if (c.peer[i].name[k].kind == 3 && !strcmp(c.peer[i].name[k].s, "_airplay")) ok = 1;
    check("service TYPE is recorded (identifies device kind without a name)", ok,
          peer_names(&c, MAC_A));
  }

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
