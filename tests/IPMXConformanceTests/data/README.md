# IPMX conformance golden media

- `ipmx_golden.h264` is an Annex B H.264 High/4:2:0/8-bit VBR-HRD stream.
- `ipmx_golden.pcap` is a LINKTYPE_RAW PCAP containing its multicast RTP packets.

The fixtures are validated semantically rather than compared byte-for-byte so x264 implementation
details, RTP randomization and capture timestamps do not make the regression test brittle.

Regenerate intentionally from a completed build with:

```powershell
./scripts/regenerate-goldens.ps1
```
