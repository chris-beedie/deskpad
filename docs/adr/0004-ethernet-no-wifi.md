# Ethernet, not WiFi

Deskpad uses the ESP32-P4-NANO's EMAC + external PHY for network
connectivity; the WiFi radio is unused. This diverges from the
kvm-switcher heritage firmware, which was WiFi-only with an
open-AP captive-portal first-boot.

Ethernet was chosen for:

- **Stability of Live-Key rendering.** State-display keys subscribe to
  MQTT topics; latency and disconnects degrade the UX visibly.
- **Predictability around work laptops.** The desk is in
  corporate-WiFi-adjacent territory; a wired link sidesteps WPA2-
  Enterprise / 802.1X / VLAN complications.
- **Lower setup friction in normal use.** The deskpad sits on a desk
  next to a wired switch.

The bootstrap consequence: there is **no AP-fallback for first-time
setup.** First boot announces `deskpad.local` via mDNS over DHCP; the
SPA detects an empty NVS config and shows a setup wizard. As a
robustness fallback the device IP is rendered onto an LCD key during
first boot so the user can read it directly if mDNS is unavailable.
