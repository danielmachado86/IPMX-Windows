# IPMX conformance golden media

- `ipmx_golden.h264` is an Annex B H.264 High/4:2:0/8-bit VBR-HRD stream.
- `ipmx_golden.pcap` is a LINKTYPE_RAW PCAP containing multicast RTP on port 5004 and the
  corresponding per-frame IPMX RTCP Sender Reports on port 5005.

The media fixtures are validated semantically so x264 implementation details, RTP randomization and
capture timestamps do not make the regression test brittle. A deterministic RTCP compound packet is
also compared byte-for-byte in `IPMXCoreTests`, including Media Info Blocks 0x0005 and 0x000A.

Regenerate intentionally from a completed build with:

```powershell
./scripts/regenerate-goldens.ps1
```

To validate the PCAP with the IPMX Lua dissector and Wireshark/tshark:

```powershell
./scripts/verify-ipmx-dissector.ps1 `
  -DissectorPath C:\path\to\ipmx-rtcp-info-dissector\ipmx_rtcp_info.lua
```
