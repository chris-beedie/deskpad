# Hosts are U38 input slots, not Machines

Three laptops are in active use, but deskpad models only two **Hosts** —
one per U38 input. The shared Dock B slot holds either Personal or
Secondary Work depending on which laptop is currently cable-swapped in,
and deskpad does not try to distinguish them at the protocol level.

Routing (DDC, HID Bridge, MQTT discovery, binding `scope`) keys off the
**slot** because:

- No reliable host-identity signal exists for an un-agented Windows
  work laptop (a Host Agent might never run there).
- The user's workflow already implies physical disambiguation — they
  know which laptop they just plugged in.
- A two-Host abstraction matches the only thing the U38 actually
  exposes (VCP 0x60 input values), keeping the schema flat.

A separate **Machine** concept may be re-introduced later when/if a
Host Agent runs on multiple machines and software-level disambiguation
becomes useful.
