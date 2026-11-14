/*
 * Copyright 2012-2015 Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

import QtQuick 2.4
import Lomiri.Components 1.3 as Toolkit
import Lomiri.PerformanceMetrics 1.0
import QtQuick.Window 2.2

/*!
    \qmltype MainView
    \inqmlmodule Lomiri.Components
    \ingroup lomiri
    \inherits StyledItem
    \brief MainView is the root Item that should be used for all applications.

    The simplest way to use a MainView is to include a single \l Page object
    inside the MainView:
    \qml
        import QtQuick 2.4
        import Lomiri.Components 1.3

        MainView {
            width: units.gu(48)
            height: units.gu(60)

            Page {
                header: PageHeader {
                    id: pageHeader
                    title: "Simple page"
                }
                Button {
                    anchors {
                        horizontalCenter: parent.horizontalCenter
                        top: pageHeader.bottom
                        topMargin: units.gu(5)
                    }
                    width: units.gu(15)
                    text: "Push me"
                    onClicked: print("Click!")
                }
            }
        }
    \endqml
    It is not required to set the anchors of the \l Page as it will automatically fill its parent.

    Do not include multiple Pages directly inside the MainView, but use \l AdaptivePageLayout
    inside MainView to navigate between several Pages.

    If the \l Page inside the MainView includes a Flickable, set the flickable property of
    the PageHeader to automatically hide and show the header when the user scrolls up or down:
    \qml
        import QtQuick 2.4
        import Lomiri.Components 1.3

        MainView {
            width: units.gu(48)
            height: units.gu(60)

            Page {
                header: PageHeader {
                    title: "Page with Flickable"
                    flickable: myFlickable
                }

                Flickable {
                    id: myFlickable
                    anchors.fill: parent
                    contentHeight: column.height

                    Column {
                        id: column
                        Repeater {
                            model: 100
                            Label {
                                text: "line "+index
                            }
                        }
                    }
                }
            }
        }
    \endqml
    The same header behavior is automatic when using a ListView instead of a Flickable in the above
    example.

    The examples above show how to include a single \l Page inside a MainView, but more
    advanced application structures are possible using \l AdaptivePageLayout.
*/
Toolkit.MainViewBase {
    id: mainView

    /*!
      \internal
      Hint the default width of the MainView
     */
    implicitWidth: units.gu(45)
    /*!
      \internal
      Hint the default height of the MainView
     */
    implicitHeight: units.gu(75)

    /*! \deprecated */
    property bool automaticOrientation: false

    /*!
      \internal
      Use default property to ensure children added do not draw over the header.
     */
    default property alias contentsItem: contents.data
    Item {
        id: canvas
        anchors {
            fill: parent
            bottomMargin: mainView.anchorToKeyboard &&
                LomiriApplication.inputMethod.visible ?
                    LomiriApplication.inputMethod.keyboardRectangle.height : 0
        }

        Item {
            id: contents
            anchors {
                fill: parent
            }
        }

        /*!
          The header of the MainView. Can be used to obtain the height of the header
          in \l Page to determine the area for the \l Page to fill.
         */
        AppHeader {
            // This objectName is used in the MainView autopilot custom proxy object
            // in order to select the application header.
            // Also used in tst_header_locked_visible.qml.
            objectName: "MainView_Header"
            id: headerItem
            property real bottomY: headerItem.y + headerItem.height
            dividerColor: Qt.darker(mainView.headerColor, 1.1)
            panelColor: Qt.lighter(mainView.headerColor, 1.1)
            backgroundColor: mainView.headerColor

            title: internal.activePage ? internal.activePage.title : ""
            pageStack: internal.activePage ? internal.activePage.pageStack : null

            contents: internal.activePage &&
                      internal.activePage.hasOwnProperty("__customHeaderContents") ?
                          internal.activePage.__customHeaderContents : null

            Toolkit.PageHeadConfiguration {
                id: defaultConfig
                // Used when there is no active Page, or a Page 1.0 is used which
                // does not have a PageHeadConfiguration.
            }
            config: visible && internal.activePage &&
                    internal.activePage.hasOwnProperty("head") ?
                        internal.activePage.head : defaultConfig

            // don't show the application header if the page has its own header.
            visible: !(internal.activePage &&
                       internal.activePage.hasOwnProperty("header") &&
                       internal.activePage.header) &&
                     internal.activePage

            height: visible ? implicitHeight : 0

            // 'window' is defined by QML between startup and showing on the screen.
            // There is no signal for when it becomes available and re-declaring it is not safe.
            property bool windowActive: window !== null
            function updateWindowTitle() {
                if (window) {
                    window.title = headerItem.title;
                }
            }
            onWindowActiveChanged: {
                updateWindowTitle();
            }
            onTitleChanged: {
                updateWindowTitle();
            }
        }

        Connections {
            target: Qt.application
            function onActiveChanged() {
                if (Qt.application.active) {
                    if (!(headerItem.config &&
                          headerItem.config.hasOwnProperty("locked") &&
                          headerItem.config.locked)) {
                        headerItem.exposed = true;
                    }
                }
            }
        }
    }

    Item {
        id: internal

        // Even when using MainView 1.1, we still support Page 1.0.
        // PageBase (=Page 1.0) is the superclass of Page 1.1.
        property Item activePage: isPage(mainView.activeLeafNode) ? mainView.activeLeafNode : null

        function isPage(item) {
            return item && item.hasOwnProperty("__isPageTreeNode") &&
                    item.__isPageTreeNode &&
                    item.hasOwnProperty("title") &&
                    item.hasOwnProperty("flickable") &&
                    item.hasOwnProperty("active") &&
                    item.hasOwnProperty("pageStack")
        }
    }

    __propagated: QtObject {
        /*!
          \internal
          The header that will be propagated to the children in the page tree node.
          It is used by Tabs to bind header's tabsModel.
         */
        property AppHeader header: headerItem

        /*!
          \internal
          The action manager that has the global context for the MainView's actions,
          and to which a local context can be added for each Page that has actions.actions.
         */
        property var actionManager: mainView.actionManager

        /*!
          \internal
          Used by PageStack. This property only exists in MainView 1.2 and later.
         */
        readonly property bool animateHeader: false
    }

    backgroundColor: theme.palette.normal.background

    // ---- Larry port: Android-style back gesture (upper half of the left edge) ----
    // Lomiri's launcher edge is limited to the lower half on this device; a swipe in
    // from the upper half lands here and triggers the current page's back action.
    function __larryFindBack(item, best) {
        if (!item || !item.visible)
            return best;
        try {
            if (item.hasOwnProperty("navigationActions") && item.navigationActions
                    && item.navigationActions.length > 0) {
                var a = item.navigationActions[0];
                if (a && a.visible && a.enabled) {
                    var px = item.mapToItem(mainView, 0, 0).x;
                    if (!best || px >= best.x)
                        best = { x: px, action: a, button: null };
                }
            }
            if ((item.objectName === "backButton" || item.objectName === "customBackButton")
                    && item.enabled && typeof item.trigger === "function") {
                var bx = item.mapToItem(mainView, 0, 0).x;
                if (!best || bx >= best.x)
                    best = { x: bx, action: null, button: item };
            }
        } catch (e) {}
        var kids = item.children;
        for (var i = 0; i < kids.length; i++)
            best = __larryFindBack(kids[i], best);
        return best;
    }

    function __larryGoBack() {
        var best = __larryFindBack(mainView, null);
        if (!best)
            return false;
        if (best.action)
            best.action.trigger();
        else
            best.button.trigger();
        return true;
    }

    Toolkit.SwipeArea {
        id: __larryBackEdge
        objectName: "larryBackEdge"
        direction: Toolkit.SwipeArea.Rightwards
        anchors { left: parent.left; top: parent.top }
        // Same trigger width as the shell's edges: com.lomiri.Shell edge-drag-width
        // (grid units, System Settings). Read dynamically so a missing module or a
        // sandbox that blocks dconf just falls back to the default of 3.
        property var __shellSettings: null
        width: units.gu(__shellSettings && __shellSettings.edgeDragWidth > 0
                        ? __shellSettings.edgeDragWidth : 3)
        height: parent.height / 2
        Component.onCompleted: {
            try {
                __shellSettings = Qt.createQmlObject(
                    'import GSettings 1.0; GSettings { schema.id: "com.lomiri.Shell" }',
                    __larryBackEdge, "larryEdgeSettings");
            } catch (e) {
                __shellSettings = null;
            }
        }
        z: 100000
        readonly property real threshold: units.gu(8)
        onDraggingChanged: {
            if (!dragging && distance >= threshold)
                mainView.__larryGoBack();
        }
        Rectangle {
            visible: __larryBackEdge.dragging
            width: units.gu(5)
            height: width
            radius: width / 2
            color: __larryBackEdge.distance >= __larryBackEdge.threshold
                   ? theme.palette.normal.focus : theme.palette.normal.foreground
            opacity: 0.9
            x: Math.min(__larryBackEdge.distance, __larryBackEdge.threshold) - width
            y: __larryBackEdge.touchPosition.y - height / 2
            Toolkit.Icon {
                anchors.centerIn: parent
                width: units.gu(2.5)
                height: width
                name: "back"
                color: theme.palette.normal.foregroundText
            }
        }
    }
    // ---- end Larry port ----
}
