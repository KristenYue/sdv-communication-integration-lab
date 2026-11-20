# Bounded SOME/IP request/response subset

This project implements a deliberately bounded SOME/IP wire path for one vehicle-state method. It follows the AUTOSAR Foundation R23-11 base header layout but does not claim AUTOSAR conformance or interoperability with a production SOME/IP stack.

Primary specification reference: [AUTOSAR Foundation R23-11 SOME/IP Protocol Specification](https://www.autosar.org/fileadmin/standards/R23-11/FO/AUTOSAR_FO_PRS_SOMEIPProtocol.pdf).

## Implemented wire contract

- 16-bit Service ID and 16-bit Method ID;
- 32-bit Length covering Request ID through the end of the payload;
- 16-bit Client ID and 16-bit Session ID;
- Protocol Version `0x01` and Interface Version `0x01`;
- Request (`0x00`), Response (`0x80`) and Error (`0x81`) message types;
- selected return codes for unknown service, unknown method and malformed message;
- big-endian header and project payload fields;
- UDP request/response on loopback with request-identity correlation;
- a 4 KiB project payload boundary and exact datagram-length validation.

Service `0x1234`, method `0x0001` maps to `vehicle.state/GetSnapshot`. Its request payload is two signed 64-bit values (`now_ms`, `max_age_ms`). Its response payload is a project-specific five-byte schema: speed in 0.01 km/h, battery percentage, door-open flag and an all-signal-groups-fresh flag.

## Explicitly not implemented

- SOME/IP Service Discovery;
- events, event groups, subscriptions or notifications;
- TCP transport, segmentation or SOME/IP-TP;
- E2E profiles;
- AUTOSAR serialization for arbitrary data types;
- authentication, encryption, ACL distribution or Secure SOME/IP;
- multicast, VLAN, TSN or physical Automotive Ethernet;
- interoperability or conformance testing against vsomeip or an OEM stack.

The localhost path proves that two independent C++ processes exchange a correctly bounded request and response through the service runtime. It does not prove behavior on an in-vehicle Ethernet network.
