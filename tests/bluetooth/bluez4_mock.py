"""Fake BlueZ 4.101 bluetoothd for tests/bluetooth (python-dbusmock template).

What the player reads and calls on BlueZ 4 (doc/manager-api.txt,
adapter-api.txt, device-api.txt, audio-api.txt, media-api.txt of 4.101):
org.bluez.Manager.DefaultAdapter on "/", GetProperties on the adapter,
devices and AudioSink, and org.bluez.Media.RegisterPlayer /
UnregisterPlayer on the adapter. No ObjectManager: that is how BlueZ 4 is
told apart from BlueZ 5. Calls are recorded by dbusmock (GetMethodCalls).
"""

import dbus  # noqa: F401 (used by the method code strings)
from dbusmock import mockobject

BUS_NAME = "org.bluez"
MAIN_OBJ = "/"
MAIN_IFACE = "org.bluez.Manager"
SYSTEM_BUS = True

ADAPTER = "/org/bluez/1234/hci0"
HEADPHONES = ADAPTER + "/dev_AA_BB_CC_DD_EE_02"
PHONE = ADAPTER + "/dev_AA_BB_CC_DD_EE_03"
A2DP_SINK = "0000110b-0000-1000-8000-00805f9b34fb"
A2DP_SOURCE = "0000110a-0000-1000-8000-00805f9b34fb"


def load(mock, parameters):
    mock.AddMethods(
        MAIN_IFACE,
        [
            ("DefaultAdapter", "", "o", f"ret = dbus.ObjectPath('{ADAPTER}')"),
        ],
    )
    devices = f"dbus.Array([dbus.ObjectPath('{HEADPHONES}'), dbus.ObjectPath('{PHONE}')], signature='o')"
    mock.AddObject(
        ADAPTER,
        "org.bluez.Adapter",
        {},
        [
            (
                "GetProperties",
                "",
                "a{sv}",
                "ret = {'Powered': dbus.Boolean(True), 'Name': 'r3pro', 'Devices': " + devices + "}",
            ),
        ],
    )
    mockobject.objects[ADAPTER].AddMethods(
        "org.bluez.Media",
        [
            ("RegisterPlayer", "oa{sv}a{sv}", "", ""),
            ("UnregisterPlayer", "o", "", ""),
        ],
    )
    mock.AddObject(
        HEADPHONES,
        "org.bluez.Device",
        {},
        [
            (
                "GetProperties",
                "",
                "a{sv}",
                "ret = {'Address': 'AA:BB:CC:DD:EE:02', 'Name': 'Headphones', 'Alias': 'My Headphones', "
                "'Paired': dbus.Boolean(True), 'Trusted': dbus.Boolean(True), "
                f"'UUIDs': dbus.Array(['{A2DP_SINK}'], signature='s')}}",
            ),
        ],
    )
    mockobject.objects[HEADPHONES].AddMethods(
        "org.bluez.AudioSink",
        [("GetProperties", "", "a{sv}", "ret = {'State': 'playing', 'Connected': dbus.Boolean(True)}")],
    )
    mock.AddObject(
        PHONE,
        "org.bluez.Device",
        {},
        [
            (
                "GetProperties",
                "",
                "a{sv}",
                "ret = {'Address': 'AA:BB:CC:DD:EE:03', 'Name': 'Phone', "
                f"'Paired': dbus.Boolean(False), 'UUIDs': dbus.Array(['{A2DP_SOURCE}'], signature='s')}}",
            ),
        ],
    )
