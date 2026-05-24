# U38 is the KVM; deskpad coordinates it

The Dell U3823DW already owns USB peripheral switching across hosts via
its built-in USB hub: when its input changes, the keyboard, mouse, and
dock peripherals re-route automatically. Deskpad therefore defines
itself as a *coordinator* of the U38, not a USB-switching device of its
own — its KVM role is to drive DDC/CI into the U38 (to initiate an
input change) and to follow the U38 onto the Secondary Monitor (Dell
U2419H) so both monitors swap together.

This lets us reuse a high-end monitor's USB switching matrix instead of
routing high-speed USB signals through deskpad's own electronics, and
re-frames the project from "DIY KVM" to "control surface for an
existing-hardware KVM."

The repo README still describes deskpad as a KVM; it predates this
decision and needs rewriting.
