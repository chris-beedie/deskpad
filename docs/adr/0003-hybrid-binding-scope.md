# Hybrid binding scope (global vs host:<id>)

Each Binding carries a `scope` field of either `global` or
`host:<input>`. **Global** bindings (KVM toggle, HA-controlled lights,
ambient sensors) render on every Page regardless of Active Host.
**Host-scoped** bindings (HID chords for host-specific apps,
host-specific automations) render only when their target Host is
active.

The two pure alternatives were both untenable:

- Pure-global is too rigid: no way to label an app-specific chord
  differently per host, and HID-keystroke labels would lie about what
  they trigger when the wrong host is active.
- Pure-per-host is unsafe: if all keys were host-scoped, you'd have
  no way to reach Host A from Host B (the KVM-toggle key has to be
  reachable from every state).

The hybrid is the minimum complexity that satisfies both constraints.
The cost is one schema field and one filter pass on Active-Host change;
the Web UI exposes scope as a per-slot toggle.
