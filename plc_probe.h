#ifndef PLC_PROBE_H
#define PLC_PROBE_H

// =====================================================================
//  KNOCK BEFORE PUSHING: a bounded TCP probe of the PLC.
//
//  THE PROBLEM IT SOLVES
//
//  eip.begin() opens a TCP connection whose connect CANNOT be bounded
//  from the call site: MbedClient applies its socket timeout only AFTER
//  the connection succeeds, so with no PLC answering it runs the stack's
//  full SYN retry sequence. Measured on the machine 2026-09-03: when the
//  PLC was switched off, loopStallMs recorded 27,735 ms stuck in
//  eip.begin() (stallWhere=5). That is 85% of the 32.76 s watchdog window
//  -- the old firmware would have reset there -- and every other thing
//  the gateway does, cloud included, stopped for those 28 seconds.
//
//  WHY A SEPARATE SOCKET WITH ITS OWN TIMEOUT WORKS WHERE eip DOES NOT
//
//  The reason eip.begin() cannot be bounded is that MbedClient's socket
//  is blocking during the connect and only made non-blocking afterwards.
//  So the fix is NOT another MbedClient/EthernetClient -- that would
//  inherit the same 28 s. It is a raw mbed TCPSocket with set_timeout()
//  called BEFORE connect(), which the netsocket layer does honour on the
//  connect itself. Knock with a 300 ms deadline; only if the knock is
//  answered do we let eip.begin() run, and by then the port is known
//  open so its connect returns in milliseconds.
//
//  It is a KNOCK, not the connection. It opens, learns "someone is
//  listening on 44818", and closes. eip owns the real session; nothing
//  here touches eip's transport or state. A production controller
//  answering an extra TCP open-and-close every few seconds is normal
//  traffic and costs it nothing.
//
//  WHAT IT DOES NOT DO
//
//  It does not make PLC data appear while the PLC is down -- that data is
//  simply absent, as it should be. It bounds how long the ABSENCE costs
//  the rest of the loop: 28 seconds becomes 0.3. The structural cure for
//  "any blocking call stops everything" is still the thread split; this
//  removes the single worst offender cheaply and without the thread
//  safety a split needs.
// =====================================================================

#include <Ethernet.h>
#include "netsocket/TCPSocket.h"
#include "netsocket/SocketAddress.h"

#ifndef PLC_PROBE_TIMEOUT_MS
#define PLC_PROBE_TIMEOUT_MS   300
#endif

//  True if something accepted a TCP connection on the PLC's EtherNet/IP
//  port within PLC_PROBE_TIMEOUT_MS. False on refuse, timeout, or any
//  setup error -- a probe that cannot tell should report "not there" and
//  let the backoff hold, never a false "there" that admits the 28 s block.
static bool plcProbe(IPAddress ip, uint16_t port) {
  NetworkInterface* net = Ethernet.getNetwork();
  if (net == nullptr) return false;

  TCPSocket sock;
  if (sock.open(net) != NSAPI_ERROR_OK) return false;

  //  BEFORE connect(), which is the whole point -- this is the bound
  //  eip.begin() could not have. Blocking with a finite timeout, so the
  //  call returns within the deadline whether the port is open, closed,
  //  or silent.
  sock.set_timeout(PLC_PROBE_TIMEOUT_MS);

  char ips[16];
  snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  SocketAddress addr(ips, port);

  nsapi_error_t rc = sock.connect(addr);
  sock.close();

  //  OK or IS_CONNECTED: the port is open. Anything else -- refused,
  //  timed out, unreachable -- is "not there" for our purposes.
  return (rc == NSAPI_ERROR_OK || rc == NSAPI_ERROR_IS_CONNECTED);
}

#endif // PLC_PROBE_H
