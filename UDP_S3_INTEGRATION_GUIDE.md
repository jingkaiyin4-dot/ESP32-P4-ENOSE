# UDP S3 Integration Guide

This document explains how an external ESP32-S3 sensor node should send UDP packets to the ESP32-P4 main board in this project.

## Goal

The ESP32-P4 dashboard listens for sensor packets from distributed ESP32-S3 nodes over UDP and updates these UI cards:

- `Node S3-A`
- `Node S3-B`

The current implementation is intentionally minimal:

- No node discovery yet
- No ACK/retry yet
- No authentication yet
- Fixed UDP port
- P4 acts as UDP receiver
- S3 acts as UDP sender

## Network Requirements

Both devices must be on the same local network:

1. ESP32-P4 connects to Wi-Fi or a phone hotspot
2. ESP32-S3 connects to the same Wi-Fi or hotspot
3. ESP32-S3 sends UDP packets to the P4 local IP address

Example:

- P4 IP: `192.168.110.236`
- UDP port: `5005`

Then the S3 should send packets to:

`192.168.110.236:5005`

## P4 Receiver Details

Current P4 UDP settings:

- Protocol: UDP
- Port: `5005`
- Receiver: ESP32-P4

P4 starts the UDP listener after entering the dashboard screen.

P4 serial log on success:

```text
I (...) UDP: Listening for S3 sensor packets on UDP 5005
```

When a valid packet is received:

```text
I (...) UDP: RX node=S3_A voc=1.7 co2=520 eth=0.8
```

If the packet format is wrong:

```text
W (...) UDP: Ignored packet: ...
```

## Supported Packet Formats

The P4 currently accepts either JSON or a simple key-value text format.

### Format A: JSON

Recommended if you want clean and readable debugging.

Example for node A:

```json
{"node":"S3_A","voc":1.7,"co2":520,"eth":0.8}
```

Example for node B:

```json
{"node":"S3_B","voc":2.3,"co2":680,"eth":1.1}
```

The P4 receiver also accepts extra fields after `eth` and will ignore them.

Example with debug fields:

```json
{"node":"S3_A","voc":1.7,"co2":520,"eth":0.0,"class":"fruit","conf":93,"fresh":2}
```

In that case, the P4 currently uses only:

- `node`
- `voc`
- `co2`
- `eth`

### Format B: Key-value text

Useful for very simple sender implementations.

Example for node A:

```text
node=S3_A,voc=1.7,co2=520,eth=0.8
```

Example for node B:

```text
node=S3_B,voc=2.3,co2=680,eth=1.1
```

## Supported Node IDs

The P4 currently maps these names:

### Node A

- `S3_A`
- `S3A`
- `Node S3-A`

### Node B

- `S3_B`
- `S3B`
- `Node S3-B`

To avoid ambiguity, use:

- `S3_A`
- `S3_B`

## Data Mapping

Each packet should contain:

- `node`: node ID string
- `voc`: floating-point VOC value in ppm
- `co2`: integer CO2 value in ppm
- `eth`: floating-point Ethylene value in ppm

Example:

```json
{"node":"S3_A","voc":1.7,"co2":520,"eth":0.8}
```

This updates the P4 dashboard to:

- `VOC: 1.7 ppm`
- `CO2: 520 ppm`
- `Ethylene: 0.8 ppm`

## Sending Frequency

Recommended initial frequency:

- Once every `500 ms` to `1000 ms`

Suggested starting point:

- `1000 ms`

This is enough for UI refresh and keeps traffic small.

## S3 Sender Behavior Recommendation

Recommended behavior on the S3 side:

1. Connect to Wi-Fi/hotspot
2. Wait until it gets an IP
3. Know the P4 target IP address
4. Send one UDP packet every second
5. Use a stable node name such as `S3_A` or `S3_B`

## Minimal Sender Pseudocode

```c
connect_wifi("your_ssid", "your_password");

while (!wifi_has_ip()) {
    delay_ms(100);
}

udp_socket = udp_open();

while (1) {
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"node\":\"S3_A\",\"voc\":%.1f,\"co2\":%d,\"eth\":%.1f}",
             voc_value, co2_value, eth_value);

    udp_sendto(udp_socket, "192.168.110.236", 5005, payload, strlen(payload));
    delay_ms(1000);
}
```

## Example Test Payloads

Use these first before integrating real sensors.

### Test node A

```json
{"node":"S3_A","voc":1.7,"co2":520,"eth":0.8}
```

### Test node B

```json
{"node":"S3_B","voc":2.4,"co2":610,"eth":1.2}
```

## P4 UI Expectations

Before any UDP packet arrives, the node cards show:

- `VOC: waiting UDP`
- `CO2: waiting UDP`
- `Ethylene: waiting UDP`

After a valid packet arrives, the corresponding node card updates immediately.

## Current Limitations

Current version limitations:

1. P4 does not automatically discover S3 nodes
2. S3 must already know the P4 IP address
3. No packet acknowledgement
4. No timeout/offline indicator yet
5. Only `S3_A` and `S3_B` are mapped to UI cards

## Recommended Next Steps

After the first sender works, the next improvements should be:

1. Add P4 UDP broadcast discovery
2. Add node online/offline timeout
3. Add packet version field
4. Add optional checksum or sequence number
5. Add command packets from P4 to S3

## Quick Checklist For S3 Developer

Before debugging, verify:

1. S3 and P4 are on the same Wi-Fi/hotspot
2. P4 has entered the dashboard screen
3. P4 serial log shows UDP listener started
4. S3 sends to the correct P4 IP
5. UDP destination port is `5005`
6. Packet uses one of the supported formats
7. Node ID is `S3_A` or `S3_B`

## Summary

The current integration contract is simple:

- Transport: UDP
- P4 port: `5005`
- Sender: ESP32-S3
- Receiver: ESP32-P4
- Payload: JSON or `key=value` text
- Required fields: `node`, `voc`, `co2`, `eth`
- Extra JSON fields are allowed and ignored by the current P4 parser

Recommended production-facing sender payload:

```json
{"node":"S3_A","voc":1.7,"co2":520,"eth":0.8}
```
