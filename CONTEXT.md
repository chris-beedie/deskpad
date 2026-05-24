# Deskpad

Deskpad is a desk control surface that coordinates a multi-host workstation
centred on a single KVM-capable monitor. It surfaces six LCD-backed buttons
and a rotary encoder for switching hosts, controlling Home Assistant,
displaying live status, and (in time) sending richer-than-HID commands to
the currently-active host.

## Language

**Deskpad**:
The device and firmware as a whole — the ESP32-P4-NANO control board plus
its attached AKP03E LCD pad and HID Bridge. Not a KVM itself.
_Avoid_: "the KVM", "the controller", "the box"

**KVM**:
The Dell U3823DW monitor. Owns USB peripheral switching across Hosts:
when its input changes, its built-in USB hub re-routes the keyboard,
mouse, and any other downstream peripherals to the matching Host.
Currently two inputs are wired: DisplayPort (Dock A → Primary Work,
fixed) and USB-C (Dock B → Personal or Secondary Work, cable-swap).
HDMI is unused.
_Avoid_: "the monitor" (ambiguous — there is a Secondary Monitor too), "the
U38" in docs (fine in chat).

**Host**:
A KVM input slot, identified by the U38's VCP `0x60` input value. There
are exactly two: Dock A (fixed: Primary Work) and Dock B (shared:
Personal or Secondary Work, cable-swap). Deskpad addresses by Host (the
slot), not by Machine.
_Avoid_: "PC", "machine", "endpoint" — those refer to physical computers,
which deskpad does not track individually.

**Machine**:
A physical laptop in the workstation. Three exist today: Primary Work,
Personal, Secondary Work. Deskpad does not identify Machines directly;
it only knows which Host (slot) is active. A Machine may run a Host
Agent, but the Agent registers itself by Host, not by Machine identity.

**Active Host**:
The Host whose USB-C input is currently selected on the KVM. It is the only
Host that receives HID Bridge events at a given moment. Deskpad tracks
which Host is active so it can address commands and adapt its UI.

**Secondary Monitor**:
The non-KVM monitor on the desk — a Dell U2419H. Doesn't switch
automatically when the KVM switches; deskpad follows the KVM by driving
DDC/CI on this monitor over its own I2C bus.

**DDC physical topology** (non-obvious — not derivable from code):
- The U38 is *driven* on DisplayPort and USB-C, but deskpad reaches
  its DDC channel via the U38's otherwise-unused HDMI port. None of
  the U38's HDMI inputs carry video — they're DDC-only taps.
- The U24's HDMI input 1 *does* carry video as a pass-through, and
  deskpad taps DDC on the same connector.
- In the heritage S3 firmware, monitor-awake detection
  (`ddc_monitor_awake`, VCP 0xD6) ran against the U24 rather than the
  U38. Whether the U38's HDMI-DDC tap would have answered the same
  query reliably is untested. To verify when Phase 6 (external KVM
  detection) lands.

**PC1 / PC2**:
The deskpad-level abstraction of "which Host is active." `kvm_toggle`
flips between them. The concrete VCP `0x60` values per monitor are in
`deskpad-host/main/ddc.h`:
- PC1: U38 = DP (0x0F), U24 = HDMI 1 (0x11) — Primary Work via Dock A.
- PC2: U38 = USB-C (0x1B), U24 = DP (0x0F) — Personal or Secondary
  Work via Dock B (cable-swap).

**HID Bridge**:
A Seeed Xiao RP2350 board that appears to the Active Host as a USB HID
keyboard + consumer-control device. Receives key/command intents from the
deskpad host firmware over a UART link and replays them to whatever Host
the KVM has currently selected. Long-term may be replaced by an internal
USB FS controller on the P4-NANO.
_Avoid_: "the keyboard board", "the second board"

**AKP Key** (a.k.a. **LCD Key**):
One of the six 60×60 LCD-backed mechanical buttons on the AKP03E. Each
shows a JPEG and animates (shrinks + darkens) when pressed.

**Side Key**:
One of the three plain (non-LCD) buttons below the AKP keys. Each one is
hard-wired to jump to a specific Page (side key N → page N). No
prev/next traversal.

**Page**:
A set of six Slot bindings rendered together on the AKP keys. There are
exactly three Pages. Side Keys jump directly to a Page.

**Slot**:
A position 0..5 on a Page that holds one Binding. Each Slot is rendered
to one AKP Key. Pages also hold Encoder Bindings (see Encoder).

**Encoder**:
A rotary control. The AKP03E has three: one large central knob plus two
smaller knobs. Each generates left, right, and press events. Encoder
behaviour is bound per Page (the same way Slot Bindings are), with the
expectation that most users will copy one binding across all three Pages
for muscle-memory consistency.

**Binding**:
What a Slot does. Currently three types: `kvm_toggle` (flip the Active
Host between Dock A and Dock B), `hid` (send a key chord via the HID
Bridge), `ha` (call an HA service, optionally subscribe to entity state
for display).
A Binding has a `scope` of either `global` (active on every Active Host)
or `host:<id>` (active only when a specific Host is active).

**Renderer**:
A built-in firmware function that draws an AKP Key's content into an
LVGL canvas given its config and bound state. Renderers are referenced
by name in a Binding's `display` field (e.g. `"bulb"`, `"thermostat"`,
`"monitor"`, `"clock"`, `"static_jpeg"`). The renderer also declares
what state subscriptions it needs (MQTT topics, Active-Host changes,
timers), and the pipeline re-renders the key when any of those fire.

**Live Key**:
Any AKP Key whose Renderer produces content that varies over time or
state. The clock is one example; an HA-bound light bulb is another.
A purely-static key is just a Live Key whose Renderer happens to ignore
state — they share one render pipeline.

**Networking**:
Deskpad is on the LAN via wired Ethernet (P4-NANO's EMAC + PHY), not
WiFi. DHCP for IPv4; mDNS for discovery as `deskpad.local`. There is no
WiFi captive-portal fallback as in the old kvm-switcher firmware.

**Host Agent**:
A small process running on a Host that receives commands from deskpad that
can't be expressed as raw HID — e.g. "open the same Outlook search in
whichever window is foreground". On Windows work laptops this will be
PowerShell. Optional; Hosts without an agent are still controllable via
HID Bridge.

## Relationships

- A **Deskpad** coordinates exactly one **KVM**
- A **KVM** has many **Hosts**, of which exactly one is the **Active Host**
- Each **Host** may run zero or one **Host Agent**
- A **Deskpad** has six **AKP Keys**; any subset may be **Live Keys**
- A **Deskpad** has one **HID Bridge**; the Bridge addresses the **Active Host**
- A **Deskpad** follows the **KVM** onto the **Secondary Monitor** via DDC/CI

## Example dialogue

> **Dev:** "If a user holds an **AKP Key** that triggers a macro on the
> **Active Host**, and mid-press the KVM switches input, where do the
> remaining keystrokes go?"
> **Domain expert:** "Whoever is the **Active Host** at the moment each
> HID report leaves the **HID Bridge**. The Bridge has no notion of a
> 'press' lasting longer than a single report — switching the KVM
> mid-press just means the second half of the macro lands on the new
> Host. We'll make deskpad cancel in-flight macros on switch."

## Flagged ambiguities

- The repo README still describes deskpad as "a KVM"; resolved by
  [ADR-0001](docs/adr/0001-u38-is-the-kvm.md) — *KVM = the U38*. The
  README needs to be rewritten to match. Open follow-up.
- "command" vs "keypress" is not yet pinned down — both end up on the
  Active Host but via different paths (HID Bridge vs Host Agent). To be
  refined when the Host Agent transport is decided (deferred from v1).
- Host vs Machine: resolved by
  [ADR-0002](docs/adr/0002-host-is-kvm-slot-not-machine.md). A Host is a
  slot; a Machine is a computer. For Dock B, two Machines share one Host.
  Deskpad addresses by Host until a future feature forces otherwise.
