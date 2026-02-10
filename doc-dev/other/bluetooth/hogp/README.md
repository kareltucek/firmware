Yo# HOGP: c2usb ↔ Zephyr Integration

How the UHK's HID-over-GATT (HOGP) works, from BLE connection to keystrokes.

**Key source files:**

| Layer | File | Role |
|-------|------|------|
| GATT wrapper | `c2usb/port/zephyr/bluetooth/gatt.hpp` | Thin C++ wrapper over Zephyr's `bt_gatt_*` API |
| HID service | `c2usb/port/zephyr/bluetooth/hid.hpp` / `hid.cpp` | HOGP service implementation |
| Firmware glue | `device/src/usb/usb.cpp` | `hogp_manager`, `HOGP_Enable()` / `HOGP_Disable()` |

---

## GATT/ATT Crash Course

> If you're unfamiliar with GATT/ATT, read [gatt-primer.md](gatt-primer.md) first.

In short: the UHK is the GATT **server**, the host (laptop/phone) is the **client**. The client discovers services and characteristics by handle, then subscribes to notifications by writing to CCC descriptors. Notifications are how keystrokes reach the host.

### ATT (Attribute Protocol)

ATT is the wire protocol underneath GATT. Everything in the attribute table above — services, characteristics, descriptors — lives as a flat array of **attributes**, each with a numeric **handle** (assigned by the stack at registration time).

```mermaid
graph TD
    subgraph "What an attribute is"
        direction LR
        H["<b>Handle</b><br/>uint16, assigned by stack<br/>e.g. 0x0012"]
        U["<b>UUID</b><br/>attribute type<br/>e.g. 0x2A4D = Report"]
        P["<b>Permissions</b><br/>READ, WRITE, etc."]
        V["<b>Value</b><br/>the actual bytes"]
    end
```

When the host talks to the device, every operation is an ATT request targeting a handle:

```mermaid
sequenceDiagram
    participant H as Host
    participant D as Device (UHK)

    Note over H, D: ATT operations used by HOGP

    H->>D: ATT Read Request (handle=0x0012)
    D-->>H: ATT Read Response (value bytes)
    Note right of D: e.g. reading Report Map

    H->>D: ATT Write Request (handle=0x0020, value=0x0001)
    D-->>H: ATT Write Response
    Note right of D: e.g. writing CCC to subscribe

    H->>D: ATT Write Command (handle=0x0018, value)
    Note right of D: no response — "write without response"<br/>e.g. writing Control Point

    D->>H: ATT Handle Value Notification (handle=0x0014, value)
    Note left of H: unsolicited push from device<br/>e.g. a keystroke input report
```

**Key differences between the ATT operations:**

| ATT operation | Direction | Response? | HOGP usage |
|---------------|-----------|-----------|------------|
| Read Request / Response | Host → Device | Yes | Report Map, HID Info, current report value |
| Write Request / Response | Host → Device | Yes | CCC subscription |
| Write Command | Host → Device | No | Protocol Mode set, Control Point, Output Reports |
| Notification | Device → Host | No | **Input Reports (keystrokes, mouse moves)** |

The important takeaway: **notifications are fire-and-forget from the BLE perspective**. The host never ACKs them at the ATT level. (The BLE link layer does ACK them, but that's invisible to GATT.) This is why `bt_gatt_notify_cb` has a *completion callback* rather than a *response callback* — it fires when the local stack has transmitted the packet, not when the host confirms receipt.

> [*Ask me for more*: MTU negotiation and how it affects max report size, security/encryption levels, ATT error codes]

---

## HOGP Layers (compared to USB)

On the USB side, c2usb has four layers: `function` → `device` → `mac` → `udc_mac` (see [c2usb-init.md](c2usb-init.md)). On the BLE side, the stack is **much flatter** — there's no `device`, no `mac`, no endpoint abstraction. `hid::service` talks directly to Zephyr's BT GATT API:

```mermaid
graph TB
    subgraph "USB path (for comparison)"
        direction TB
        U_APP["keyboard_app::usb_handle()"]
        U_FN["hid::function"]
        U_DEV["usb::df::device"]
        U_MAC["udc_mac"]
        U_HW["USB hardware"]
        U_APP --- U_FN --- U_DEV --- U_MAC --- U_HW
    end

    subgraph "BLE path"
        direction TB
        B_APP["keyboard_app::ble_handle()"]
        B_SVC["hid::service<br/><i>implements hid::transport</i>"]
        B_GATT["Zephyr bt_gatt_* API"]
        B_HW["BLE radio"]
        B_APP --- B_SVC --- B_GATT --- B_HW
    end
```

Why so much simpler? Because Zephyr's GATT layer already handles what `device` + `mac` do on the USB side — service registration, attribute dispatch, connection management. c2usb only needs a thin wrapper (`gatt::attribute`, `gatt::service`) around Zephyr's structs, and `hid::service` builds on top of that.

### What each layer does

| Layer | Class | Source | Role |
|-------|-------|--------|------|
| HID app | `keyboard_app`, `mouse_app`, ... | `device/src/usb/*.hpp` | Produces/consumes HID reports. **Same app classes as USB** — each has a `usb_handle()` and a `ble_handle()` returning separate instances. |
| HID transport | `hid::service` | `c2usb/.../bluetooth/hid.hpp/.cpp` | Implements the `hid::transport` interface. Builds the GATT attribute table, handles GATT callbacks, translates between `hid::application` calls and `bt_gatt_*` API calls. |
| GATT wrapper | `gatt::attribute`, `gatt::service` | `c2usb/.../bluetooth/gatt.hpp` | Thin C++ wrappers. `gatt::service` calls `bt_gatt_service_register/unregister`. `gatt::attribute::notify()` calls `bt_gatt_notify_cb`. |
| Zephyr BT | `bt_gatt_*` | Zephyr kernel | Manages connections, dispatches reads/writes to registered callbacks, sends notifications over the radio. |

### The shared interface: `hid::transport`

Both USB's `hid::function` and BLE's `hid::service` implement the same `hid::transport` interface. That's how the same `keyboard_app` works with both transports:

```mermaid
graph TD
    T["<b>hid::transport</b><br/>(abstract interface)"]
    T -->|"USB implementation"| FN["hid::function<br/>uses endpoints + control requests"]
    T -->|"BLE implementation"| SVC["hid::service<br/>uses GATT notifications + char writes"]

    APP["hid::application<br/>(keyboard_app, etc.)"]
    APP -->|"send_report()<br/>receive_report()"| T
```

The app calls `send_report()` and doesn't care whether it goes over USB or BLE — the transport handles it.

---

## Report Flows

### TX: Keystroke → Host (notification)

```mermaid
sequenceDiagram
    participant App as keyboard_app
    participant Svc as hid::service
    participant Z as Zephyr BT stack
    participant Host as Host

    App->>Svc: send_report(key_data, INPUT)<br/>(hid.cpp:348)

    Note over Svc: Look up characteristic attr<br/>for this report ID<br/>(input_report_attr(id))

    Note over Svc: Check pending_notify —<br/>if previous notify still in flight,<br/>return BUSY

    Svc->>Svc: pending_notify = data

    Svc->>Z: attr->notify(data, callback, attr, conn)<br/>→ bt_gatt_notify_cb()<br/>(gatt.hpp:224)

    Z->>Host: ATT Notification<br/>(handle, report bytes)
    Note over Host: Host receives keystroke

    Z->>Svc: completion callback<br/>(hid.cpp:393)
    Svc->>Svc: clear pending_notify
    Svc->>App: app_.in_report_sent(buf)
    Note over App: App can send<br/>the next report
```

Key details:
- Report ID byte is **stripped** before sending — GATT report characteristics don't include it (the Report Reference descriptor identifies which report it is). This is the `report_data_offset()` at `hid.hpp:175-179`.
- Only **one notification per report ID can be in flight** at a time. `pending_notify` tracks this. If the app tries to send while one is pending, it gets `BUSY`.
- The completion callback fires when **Zephyr has handed the packet to the link layer**, not when the host receives it.

### RX: Host → Device (LED report / output report)

```mermaid
sequenceDiagram
    participant Host as Host
    participant Z as Zephyr BT stack
    participant Svc as hid::service
    participant App as keyboard_app

    Host->>Z: ATT Write (Output Report char,<br/>LED data)

    Z->>Svc: set_report() callback<br/>(hid.cpp:233)

    Note over Svc: Determine report selector<br/>from Report Reference descriptor<br/>(type=OUTPUT, id=1)

    Note over Svc: Validate: reject INPUT writes,<br/>check offset/length

    Svc->>Svc: Copy to rx_buffers_[OUTPUT]<br/>prepend report ID byte

    Svc->>App: app_.set_report(OUTPUT, buffer)<br/>(hid.cpp:268)

    Note over App: keyboard_app updates<br/>Caps Lock LED, etc.

    Z-->>Host: ATT Write Response (OK)
```

Key details:
- The report ID byte is **prepended** before passing to the app — the reverse of TX stripping. The app always sees the full report with ID (`hid.cpp:264`).
- Data is copied into `rx_buffers_` to extend its lifetime beyond the Zephyr callback.
- The host can also **read** reports (GET_REPORT): the `get_report()` callback (`hid.cpp:210`) calls `app_.get_report()` to fetch the current state.

### Comparison with USB

| Aspect | USB path | BLE path |
|--------|----------|----------|
| TX mechanism | `ep_send()` on IN endpoint | `bt_gatt_notify_cb()` on characteristic |
| RX mechanism | OUT endpoint transfer | GATT write callback |
| Completion signal | `transfer_complete()` via UDC event | `bt_gatt_notify_cb` completion callback |
| Flow control | Endpoint busy flag | `pending_notify` per report ID |
| Report ID handling | Included in report data | Stripped for TX, prepended for RX (Report Reference descriptor carries it) |
| Thread context | c2usb worker thread | Zephyr BT thread (callbacks run in BT context) |

> [*Ask me for more*: how GET_REPORT works (the `get_report_buffer_` rerouting trick), Feature report handling, the `for_each()` pattern for iterating service instances]

---

## GATT Attribute Table

When `hogp_manager::select_config()` calls `hogp_nopad_.start()`, c2usb calls `bt_gatt_service_register()` (`gatt.hpp:331`), registering this attribute table with Zephyr:

```mermaid
graph LR
    subgraph "HID Service (UUID 0x1812)"
        direction TB
        S["Primary Service Declaration"]

        RM["Report Map<br/><i>READ</i><br/>HID report descriptor bytes"]
        HI["HID Information<br/><i>READ</i><br/>bcdHID, flags"]
        PM["Protocol Mode<br/><i>READ | WRITE_NO_RESP</i><br/>REPORT(1) or BOOT(0)"]
        CP["Control Point<br/><i>WRITE_NO_RESP</i><br/>Suspend / Exit Suspend"]

        subgraph "Per Input Report (IDs 1-5)"
            IR["Input Report Char<br/><i>READ | NOTIFY</i>"]
            RR["Report Reference Desc<br/><i>READ</i><br/>type=INPUT, id=N"]
            CCC["CCC Descriptor<br/><i>READ | WRITE</i><br/>host subscribes here"]
        end

        subgraph "Per Output Report (ID 1, 4)"
            OR["Output Report Char<br/><i>READ | WRITE | WRITE_NO_RESP</i>"]
            ORR["Report Reference Desc<br/><i>READ</i><br/>type=OUTPUT, id=N"]
        end

        subgraph "Per Feature Report (ID 3)"
            FR["Feature Report Char<br/><i>READ | WRITE</i>"]
            FRR["Report Reference Desc<br/><i>READ</i><br/>type=FEATURE, id=N"]
        end

        subgraph "Boot Protocol"
            BKO["Boot KB Output<br/><i>WRITE</i>"]
            BKI["Boot KB Input<br/><i>READ | NOTIFY</i>"]
            BCCC["CCC Descriptor"]
        end
    end
```

The report IDs correspond to the `multi_hid_nopad` apps:

| Report ID | Input | Output | App class |
|-----------|-------|--------|-----------|
| 1 | Keyboard 6KRO | Keyboard LEDs | `keyboard_app` |
| 2 | Keyboard NKRO | — | `keyboard_app` |
| 3 | Mouse (+ Feature) | — | `mouse_app` |
| 4 | Command | Command | `command_app` |
| 5 | Controls | — | `controls_app` |

Built by `service::fill_attributes()` at `hid.cpp:562-658`.

> [*Ask me for more*: how `report_protocol_properties` computes attribute counts at compile time, boot protocol attribute layout details]

---

## Connection Lifecycle

This is the full sequence from BLE link to active HID:

```mermaid
sequenceDiagram
    participant Host as Host (Central)
    participant Z as Zephyr BT Stack
    participant S as hid::service
    participant App as hid::application<br/>(multi_hid)

    Note over Host, Z: BLE connection established<br/>(pairing/bonding already done)

    Host->>Z: GATT Discover Services
    Z-->>Host: HID Service (0x1812) + attributes

    Host->>Z: Read Report Map
    Z->>S: get_report_map()
    S-->>Z: HID descriptor bytes
    Z-->>Host: Report Map value

    Host->>Z: Read HID Information
    Z-->>Host: bcdHID=0x0111, flags

    rect rgb(230, 245, 255)
        Note over Host, App: Subscription — this is what activates the HID app

        Host->>Z: Write CCC = 0x0001 (enable notify)<br/>on Input Report (ID 1)
        Z->>S: ccc_cfg_write()<br/>(hid.cpp:435)

        S->>S: start_app(conn, REPORT)<br/>(hid.cpp:523)
        Note over S: atomic CAS claims<br/>active_conn_

        S->>App: app_.setup(this, REPORT)
        App-->>S: success

        Z-->>Host: Write Response (OK)

        Host->>Z: Write CCC = 0x0001<br/>on remaining Input Reports (IDs 2-5)
        Note over Z, S: ccc_cfg_write() called for each,<br/>start_app() sees conn already claimed → OK
    end

    Note over Host, App: HID is now active — reports can flow

    Host->>+Z: ... normal operation ...<br/>(see "Ask me for more" below)
    Z-->>-Host: ...
```

> [*Ask me for more*: the TX and RX report flows during normal operation]

---

## Disconnection

```mermaid
sequenceDiagram
    participant Host as Host (Central)
    participant Z as Zephyr BT Stack
    participant S as hid::service
    participant App as hid::application

    Note over Host, Z: BLE link lost or host disconnects

    Z->>S: BT_CONN_CB disconnected<br/>(hid.cpp:688-691)
    S->>S: stop_app(conn)<br/>(hid.cpp:551)

    Note over S: atomic CAS clears<br/>active_conn_ → nullptr

    S->>App: app_.teardown(this)
    App-->>S: done

    Note over S, App: Service stays registered.<br/>Next connection can claim it.
```

The disconnect callback is registered globally via `BT_CONN_CB_DEFINE` (`hid.cpp:688`). It iterates all HID service instances using `for_each()` (`hid.hpp:80-94`), which walks GATT attributes looking for the `protocol_mode` characteristic (a unique marker per service instance).

---

## Single-Connection Enforcement

c2usb enforces that only **one BLE connection** can use the HID service at a time. This is done lock-free with `std::atomic<bt_conn*> active_conn_` (`hid.hpp:190`).

```mermaid
flowchart TD
    A["start_app(conn, protocol)"] --> B{"atomic CAS:<br/>active_conn_<br/>nullptr → conn"}
    B -->|Success| D["app_.setup(this, protocol)"]
    B -->|Fail: same conn| E{"Same protocol<br/>already active?"}
    E -->|Yes| F["Return true<br/>(no-op)"]
    E -->|No| G["app_.teardown(this)<br/>then app_.setup(this, new_protocol)"]
    B -->|Fail: different conn| H["Return false<br/>→ ATT error HOGP_ALREADY_CONNECTED_ERROR"]

    D --> I{"setup() succeeded?"}
    I -->|Yes| J["Return true"]
    I -->|No| K["CAS: conn → nullptr<br/>Return false"]
```

Source: `hid.cpp:523-549`.

> [*Ask me for more*: what happens when the host tries to switch between REPORT and BOOT protocol mid-session]

---

## Entry Points Summary

The two Zephyr API calls that tie everything together:

| c2usb call               | Zephyr API                         | Where          | When                                    |
| ------------------------ | ---------------------------------- | -------------- | --------------------------------------- |
| `gatt::service::start()` | `bt_gatt_service_register(this)`   | `gatt.hpp:331` | `HOGP_Enable()` → `hogp_nopad_.start()` |
| `attribute::notify()`    | `bt_gatt_notify_cb(conn, &params)` | `gatt.hpp:224` | Every input report sent to host         |

> [*Ask me for more*: the `send_report()` notification flow, output report (LED) RX flow, power suspend/resume handling, `hogp_manager` config switching]
