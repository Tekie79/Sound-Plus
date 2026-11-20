# Details and independent insert racks

The details inspector keeps separate Track Inserts and Master Inserts sections. Selecting a track changes only the track editor; Master controls retain their owner. The old rack-target dropdown is removed.

Each section is a distinct rounded surface with a consistently left-aligned disclosure, short label, subtle outline and theme accent. Space between sections and padding inside them separates controls without introducing more navigation levels. Stock and plug-in stages are visually distinct. Parameter disclosures expand only the relevant editor; the outer section remains collapsible and its existing preferences persist. Narrow layouts scroll, keeping owner labels visible rather than overlaying controls.

Icon controls retain Lucide rendering, accessible action names, keyboard focus and tooltips. Each stock/external stage has ordered slots, bypass, reorder/remove and explicit copy/move. The external rack additionally retains rack bypass, opaque preset state, generic normalized parameters and isolated editor access. Editor state applies when its window closes, with stale-owner/state rejection; the interface says so. Generic parameter gestures produce one undo history action and send controls to the retained DSP instance.

The declared boundaries are deliberate and visible: external inserts follow stock inserts; there is no shared target selector or silent cross-format replacement. Source and master bus ownership are distinct. Missing/unsupported instances retain their saved slot/state. Hosted take recording is disabled until a plugin-aware authoritative capture contract is implemented.

Native screenshots and actual interaction results belong to the accompanying implementation report; source layout alone is not visual acceptance evidence.
