# android-gestures (optional)

Android-style navigation from the left screen edge:

* upper half of the edge -> **Back** (in apps built with the Lomiri UI toolkit)
* lower half of the edge -> **app launcher**
* the trigger width follows the system `edge-drag-width` setting

Files (full copies of the 24.04-2.x originals with a marked "Larry port" block. Refresh them if
Lomiri or the UI toolkit change upstream, or they will shadow newer versions):

* `overlay/system/usr/share/lomiri/Launcher/Launcher.qml`
* `overlay/system/usr/lib/aarch64-linux-gnu/qt5/qml/Lomiri/Components/1.3/MainView.qml`

Leave out of a build: remove `android-gestures` from `extras.conf`.
