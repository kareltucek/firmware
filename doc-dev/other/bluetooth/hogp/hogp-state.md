# HOGP State Model

How c2usb's BLE HID service keeps state, and what it would take to support multiple simultaneous HID connections.

## Architecture recap

```
usb_compatibility.cpp          <-- determineSink() picks USB or BLE
        |
  keyboard_app::ble_handle()   <-- one static instance per app, per transport
  mouse_app::ble_handle()          (separate from usb_handle())
  controls_app::ble_handle()
  command_app::ble_handle()
        |
  multi_hid_nopad::handle()    <-- multi_application: dispatches to all ble_handle() apps
        |
  hogp_manager::hogp_nopad_   <-- service_instance: THE hid::service (one singleton)
        |
  Zephyr bt_gatt               <-- registered GATT attribute table
```

Each app (`keyboard_app`, `mouse_app`, ...) has **two static instances**: `usb_handle()` and `ble_handle()`. The BLE instances are wired into `multi_hid_nopad`, which is the `hid::application` bound to the single `hid::service`.

## State inside `hid::service` (one per GATT service registration)

Source: `c2usb/port/zephyr/bluetooth/hid.hpp`, `hid.cpp`

| Field | Type | What it does |
|---|---|---|
| `active_conn_` | `std::atomic<bt_conn*>` | **The** active BLE connection. Only one at a time. |
| `app_` | `hid::application&` | Reference to the multi_application (set at construction, never changes). |
| `pending_notify_[]` | `std::span<const uint8_t>[N]` | One slot per input report ID. Non-empty = notification in flight. |
| `rx_buffers_` | `reports_receiver` | One `span<uint8_t>` for OUTPUT, one for FEATURE. Filled by `receive_report()`, consumed by `set_report()`. |
| `get_report_` | `report::selector` | Transient: set during synchronous `get_report()` call, cleared immediately after. |
| `get_report_buffer_` | `span<const uint8_t>` | Transient: response data for `get_report()`. |
| `gatt_service_` | `gatt::service` | Zephyr GATT registration state (`registered_` bool). |
| `access_` | `gatt::permissions` | Computed once from security level. |
| `boot_mode_` | `boot_protocol_mode` | Compile-time constant (KEYBOARD/MOUSE/NONE). |
| CCC stores | `gatt::ccc_store[N]` | Per-characteristic subscription flags, managed by Zephyr. |

### `active_conn_` lifecycle

```
nullptr ──[client reads/writes/subscribes]──> conn    (via start_app() CAS)
conn    ──[client disconnects]──────────────> nullptr  (via stop_app() CAS)
```

Single-owner enforcement via `compare_exchange_strong`:

```cpp
// hid.cpp:532
bool service::start_app(bt_conn* conn, protocol protocol) {
    bt_conn* expected = nullptr;
    if (!active_conn_.compare_exchange_strong(expected, conn)) {
        if (expected != conn) {
            return false;  // REJECTED: another connection owns the app
        }
        // same conn, possibly different protocol → teardown + re-setup
    }
    app_.setup(this, protocol);
}
```

A second BLE host connecting to the same service gets `BT_ATT_ERR_PROCEDURE_IN_PROGRESS` on every read/write/subscribe attempt.

### `pending_notify_[]` lifecycle

One slot per input report ID (+ one for boot protocol if enabled). Acts as a "notification in flight" flag:

```
empty span ──[send_report() queues notify]──> data span    (line 394)
data span  ──[bt_gatt_notify_cb completes]──> empty span   (line 413, in callback)
```

If slot is non-empty when `send_report()` is called → returns `result::BUSY` (line 388-391). **This is the "notify busy" error.**

### `pending_notify_[]` indexing

```
Index 0: boot protocol input (if boot enabled), else report ID 1
Index 1: report ID 1 (if boot enabled), else report ID 2
Index N: report ID N (offset by 1 if boot enabled)
```

Source: `hid.cpp:332-347`

## State inside `hid::application` (one per app instance)

Source: `c2usb/hid/application.hpp`

| Field | Type | What it does |
|---|---|---|
| `transport_` | `std::atomic<transport*>` | Points to the `hid::service` that owns this app. Null when unbound. |
| `report_info_` | `report_protocol` | Report descriptor metadata (max sizes, IDs). Set at construction. |

The `transport_` pointer is how apps reach back to the service to call `send_report()` and `receive_report()`.

### `transport_` lifecycle

```
nullptr ──[setup(service, prot)]──> service*   (CAS, then calls app.start())
service ──[teardown(service)]─────> nullptr     (CAS, then calls app.stop())
```

Same CAS pattern as `active_conn_` — one transport per app at a time (`application.hpp:161-192`).

## State inside each concrete app (e.g., `keyboard_app`)

Each app instance has its own report buffers and semaphores:

| Field | Example from keyboard_app | What it does |
|---|---|---|
| `keys_` | union of boot/6kro/nkro reports | Current key state |
| `sending_sem_` | `binary_semaphore` | Flow control: acquired before send, released on completion |
| `rollover_` | `rollover` enum | N-key vs 6-key mode |
| `prot_` | `hid::protocol` | BOOT vs REPORT |
| `leds_buffer_` | output report struct | Receives LED state from host |

`app_base` (parent of mouse/controls/gamepad) has:

| Field | What it does |
|---|---|
| `in_buffer_` | `span<uint8_t>` pointing to report buffer |
| `in_id_` | `report::id` for the input report |
| `sending_sem_` | `binary_semaphore` |

## The multi_application dispatch layer

`multi_hid_nopad` (in `usb.cpp`) extends `hid::multi_application`. It holds an array of pointers to the BLE app instances:

```cpp
// usb.cpp:58
std::array<hid::application*, sizeof...(Args) + 1> app_array_{
    (&Args::ble_handle())..., nullptr
};
```

For `multi_hid_nopad` this is:
```
[0] = &keyboard_app::ble_handle()
[1] = &mouse_app::ble_handle()
[2] = &command_app::ble_handle()
[3] = &controls_app::ble_handle()
[4] = nullptr  (sentinel)
```

When the service calls `app_.setup(this, prot)`, `multi_application::start()` fans out:

```cpp
// application.hpp:228
void start(protocol prot) override {
    transport_copy_ = transport_.load();
    if (prot == protocol::BOOT) {
        apps_[0]->setup(transport_copy_, prot);  // boot: first app only
    } else {
        for (auto* app : apps_) {
            app->setup(transport_copy_, prot);    // report: ALL apps
        }
    }
}
```

Each sub-app gets its `transport_` set to point at the service. All of them share the **same** service pointer.

## What blocks multiple simultaneous HID connections

### 1. `active_conn_` is a single atomic pointer

The service can only track one `bt_conn*`. A second connection trying to interact with HOGP is rejected by `start_app()`.

### 2. Each app has one `transport_` pointer

`keyboard_app::ble_handle()` can only be bound to one transport (service) at a time. If two services tried to claim it, the second `setup()` would fail the CAS.

### 3. `pending_notify_[]` is per-service, per-report-ID

There's one "in flight" slot per report ID. If two connections needed independent notification tracking, they'd collide.

### 4. `rx_buffers_` is one buffer per report type

Only one OUTPUT and one FEATURE buffer. Two connections writing OUTPUT reports would race.

### 5. CCC subscription state is per-connection (this one is fine)

Zephyr's `ccc_store` already tracks subscriptions per-connection. This part would work with multiple connections as-is.

### 6. Report buffers in apps are shared

`keyboard_app::ble_handle()` has one `keys_` state, one `sending_sem_`. Two connections would share the same keyboard state (which might actually be desired — both hosts see the same keystrokes).

## How hard: swapping connection routing above c2usb

The idea: keep one `hid::service` active at a time, but swap which `bt_conn*` it talks to. No c2usb changes.

### What you'd need

1. **Teardown the current connection's app binding** before switching:
   ```
   service.stop_app(old_conn)  →  clears active_conn_, tears down apps
   service.start_app(new_conn) →  re-acquires with new conn, sets up apps
   ```

2. **Save/restore app-level state** if you want each host to have independent key state. For keyboard this is `keys_`, `rollover_`, `prot_`, `leds_buffer_`. For mouse it's just the report buffer.

3. **Re-subscribe CCC** — Zephyr tracks CCC per-connection, so notifications would only go to subscribed connections. After swapping `active_conn_`, the new connection's CCC state is already in Zephyr. But `pending_notify_[]` must be cleared (it may reference data for the old connection).

### What's easy

- **Connection swap itself**: `stop_app()` + `start_app()` on the service is clean, already works for disconnect/reconnect. The service was designed for this.
- **Shared keyboard state**: If both hosts see the same keystrokes (broadcast model), you don't need to save/restore `keys_` at all. Just swap the connection pointer.
- **CCC already per-connection**: Zephyr handles this.

### What's hard

- **Independent state per host**: If host A has caps lock on and host B doesn't, you need per-host `leds_buffer_`, per-host `keys_` (for 6KRO scancode slot allocation), per-host `rollover_override_`. This means either:
  - Multiple app instances (one per host) — but then you need multiple `multi_application` instances and multiple `service_instance`s, which is the "full multi-connection" approach.
  - A state snapshot/restore layer between `usb_compatibility.cpp` and the apps.
- **In-flight notifications**: If a notification is pending (`pending_notify_` non-empty) when you swap, the completion callback will fire for the old connection. You must drain or discard in-flight notifications before swapping.
- **Timing**: The swap must not race with `send_report()` calls from the main loop. Currently `usb_compatibility.cpp` calls `ble_handle().set_report_state()` from the main loop — if a swap happens mid-send, `active_conn_` could point to the wrong peer.

### Recommended approach: time-sliced swap (no c2usb changes)

```
Host A connects → start_app(connA) → notifications go to A
Host B connects → B is "parked" (connection exists but no HOGP interaction)
User switches to B → stop_app(connA), drain pending_notify, start_app(connB)
```

This is essentially what `usb_compatibility.cpp::determineSink()` already does for USB vs BLE — it picks one sink. Extending it to pick between BLE-host-A and BLE-host-B is the same pattern.

**State to save per host** (if independent state desired):

| Per-host state | Where it lives | Size |
|---|---|---|
| LED state | `keyboard_app::leds_buffer_` | 2 bytes |
| Rollover override | `keyboard_app::rollover_override_` | 1 byte |
| Protocol mode | `keyboard_app::prot_` | 1 byte |
| Scroll multiplier | `mouse_app::resolution_buffer_` | ~4 bytes |

Key report state (`keys_`) and mouse position don't need per-host storage if you're broadcasting the same input to whichever host is active.

### Alternative: true multi-connection (c2usb changes required)

Would require:
- Multiple `service_instance`s (one per host slot), each with its own `active_conn_`, `pending_notify_[]`, `rx_buffers_`
- Multiple sets of app instances (one `keyboard_app` per host)
- Multiple GATT service registrations (Zephyr supports this but each is a separate service with separate handles)
- `usb_compatibility.cpp` routes reports to all active services simultaneously

This is a much larger change and probably not worth it unless you need truly independent, simultaneous HID output to multiple BLE hosts.
