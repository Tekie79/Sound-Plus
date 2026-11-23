# Actual native-app checks

Isolated `build/hosted-automation/native-ui`, generated owned audio, ordinary Release app. No original user session loaded during QA. Native Retina Mac; screenshots are app captures, automatically downsampled for desktop (1215×768); minimum captures are1040×680 total native-window pixels/logical sizing as exposed by the tool, including titlebar. No separate1× physical display was qualified.

`minimum.jpg` / `minimum-ax.txt`: Edit at1040×680 with Browser and Inspector visible; one expanded Audio automation lane and other owners collapsed. Transport, output, toolbar and disclosure controls fit. Existing target selector/Touch controls remain in that single lane. Desktop Edit is `desktop.jpg`.

`effect-editor.jpg`: actual owned VST3 native editor of the retained audible instance. Pointer drag on its Gain slider produced a completed Touch pass. Saved `after-native-touch.soundplus` has4points versus2before; Command-Z saved `after-undo.soundplus` with2; Command-Shift-Z restored4. Save/relaunch restored the completed curve. This is agent-operated native gesture evidence, not human listening.

`four-pad-desktop.jpg` / `four-pad-minimum.jpg` / `four-pad-minimum-ax.txt`: final four-pad Perform layout. Pad B and Pad D source waveforms reach Ready after fixing foreground worker demand. All four tabs, Y/X, source pickers, macros, level/mute/solo/menu and monitor/output fit without a scroll container. Sidebars remain fixed/equal; bounded pad size follows available area. Selecting tabs does not solo them. Native Command-S from tabs saves, Space starts/stops the shared transport. Pad menu→Edit recorded automation opens the existing Master lane showing Pad D/X.

Pad B recorded Read was toggled Off, saved (`after-pad-read-off.soundplus`): A level remained Read-on/41points and B remained41points, Read-off. Toggling back restored B Read without changing points. The ordinary mute icon was toggled On then Off and its AX/model state followed. The menu fix is also exercised by the actual view-command test, including Undo.

A first automated regression assertion incorrectly expected a Pad B source in an empty test pad; its retained failure log is in stage-e. The fixture was corrected to assign B–D before the assertion, then all four affected suites passed. Actual native Pad B/D loading was independently verified above.
