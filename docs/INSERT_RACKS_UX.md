# Unified insert controls

Track Inserts and Master Inserts remain independent collapsible cards. Each card has one selector containing both stock and external effects, one Add effect action, and the selected effect's controls. There is no separate stock/external panel or shared track/master target switch.

Empty racks show only Add effect. Loaded stock effects show power, parameter disclosure and an actions menu. Plug-ins additionally show their editor action. Reset, variation locking, presets, copy/move, within-stage ordering, bulk bypass and removal remain available contextually in the actions menu; unavailable effect-specific buttons are not displayed as an empty toolbar.

Lucide plus, power and ellipsis distinguish adding, enabling and secondary actions. Power's accessible state is `Effect enabled = !bypass`; callbacks still change the model and refreshed state drives its highlight/tooltip. Parameter and outer-card disclosures retain their own expanded/collapsed state.

The shared selector follows actual processing order: stock effects precede plug-ins. This UI change does not introduce arbitrary cross-stage interleaving, alter source/master bus ownership, or change serialization/DSP. Delayed commands bind document incarnation, owner and slot. Less-used menu actions cannot redirect to a later selection.

See the [follow-up verification](INSERT_RACKS_UI_FOLLOWUP.md) for native empty/mixed states and targeted component checks. Native editors retain the existing apply-on-close policy.
