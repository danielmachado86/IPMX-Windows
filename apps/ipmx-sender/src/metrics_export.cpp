#include "ipmx/sender/metrics_export.hpp"

#include <fstream>
#include <stdexcept>

namespace ipmx::sender {
namespace {

[[nodiscard]] std::ofstream open_metrics_file(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::trunc);
  if (!output)
    throw std::runtime_error("cannot create transmitter metrics file: " + path.string());
  return output;
}

void verify_output(const std::ofstream& output) {
  if (!output)
    throw std::runtime_error("cannot write transmitter metrics file");
}

} // namespace

void write_transmitter_metrics_csv(const std::filesystem::path& path,
                                   const FrameTransmitterStats& s,
                                   const IpmxSessionTiming& timing) {
  auto output = open_metrics_file(path);
  output << "frames,packets,rtcp_reports,rtp_payload_octets,sender_report_lateness_ns,"
            "encoder_cpb_lateness_ns,packet_lateness_ns,send_jitter_ns,"
            "frame_interval_spread_ns,late_packets,ncm_violations,cpb_current_bytes,"
            "cpb_max_bytes,queued_frames,queued_frames_max,cmax,cinst_max,mmcss_registered,"
            "high_resolution_timer,timing_window_observed,encoder_delay_ns,"
            "sender_reports_delay_ns,access_unit_offset_ns\n";
  output << s.frames << ',' << s.packets << ',' << s.rtcp_reports << ',' << s.rtp_payload_octets
         << ',' << s.maximum_sender_report_lateness_ns << ',' << s.maximum_encoder_cpb_lateness_ns
         << ',' << s.maximum_packet_lateness_ns << ',' << s.maximum_send_jitter_ns << ','
         << s.maximum_interval_spread_ns << ',' << s.late_packets << ',' << s.ncm_violations << ','
         << s.current_cpb_occupancy_bytes << ',' << s.maximum_cpb_occupancy_bytes << ','
         << s.current_queued_frames << ',' << s.maximum_queued_frames << ',' << s.configured_cmax
         << ',' << s.maximum_cinst_observed << ',' << (s.mmcss_registered ? 1 : 0) << ','
         << (s.high_resolution_timer ? 1 : 0) << ',' << (s.timing_window_observed ? 1 : 0) << ','
         << timing.encoder_delay_ns << ',' << timing.sender_reports_delay_ns << ','
         << timing.access_unit_offset_ns << '\n';
  verify_output(output);
}

void write_transmitter_metrics_json(const std::filesystem::path& path,
                                    const FrameTransmitterStats& s,
                                    const IpmxSessionTiming& timing) {
  auto output = open_metrics_file(path);
  output << "{\n"
         << "  \"skipped_intervals\": " << s.skipped_intervals << ",\n"
         << "  \"encode_deadline_misses\": " << s.encode_deadline_misses << ",\n"
         << "  \"maximum_sr_interval_spread_ns\": " << s.maximum_sr_interval_spread_ns << ",\n"
         << "  \"minimum_sr_rtp_gap_ns\": " << s.minimum_sr_rtp_gap_ns << ",\n"
         << "  \"maximum_sr_send_duration_ns\": " << s.maximum_sr_send_duration_ns << ",\n"
         << "  \"maximum_rtp_send_duration_ns\": " << s.maximum_rtp_send_duration_ns << ",\n"
         << "  \"frames\": " << s.frames << ",\n"
         << "  \"packets\": " << s.packets << ",\n"
         << "  \"rtcp_reports\": " << s.rtcp_reports << ",\n"
         << "  \"rtp_payload_octets\": " << s.rtp_payload_octets << ",\n"
         << "  \"maximum_sender_report_lateness_ns\": " << s.maximum_sender_report_lateness_ns
         << ",\n"
         << "  \"maximum_encoder_cpb_lateness_ns\": " << s.maximum_encoder_cpb_lateness_ns << ",\n"
         << "  \"maximum_packet_lateness_ns\": " << s.maximum_packet_lateness_ns << ",\n"
         << "  \"maximum_send_jitter_ns\": " << s.maximum_send_jitter_ns << ",\n"
         << "  \"maximum_frame_interval_spread_ns\": " << s.maximum_interval_spread_ns << ",\n"
         << "  \"late_packets\": " << s.late_packets << ",\n"
         << "  \"ncm_violations\": " << s.ncm_violations << ",\n"
         << "  \"current_cpb_occupancy_bytes\": " << s.current_cpb_occupancy_bytes << ",\n"
         << "  \"maximum_cpb_occupancy_bytes\": " << s.maximum_cpb_occupancy_bytes << ",\n"
         << "  \"current_queued_frames\": " << s.current_queued_frames << ",\n"
         << "  \"maximum_queued_frames\": " << s.maximum_queued_frames << ",\n"
         << "  \"configured_cmax\": " << s.configured_cmax << ",\n"
         << "  \"maximum_cinst_observed\": " << s.maximum_cinst_observed << ",\n"
         << "  \"mmcss_registered\": " << (s.mmcss_registered ? "true" : "false") << ",\n"
         << "  \"high_resolution_timer\": " << (s.high_resolution_timer ? "true" : "false") << ",\n"
         << "  \"timing_window_observed\": " << (s.timing_window_observed ? "true" : "false")
         << ",\n"
         << "  \"encoder_delay_ns\": " << timing.encoder_delay_ns << ",\n"
         << "  \"sender_reports_delay_ns\": " << timing.sender_reports_delay_ns << ",\n"
         << "  \"access_unit_offset_ns\": " << timing.access_unit_offset_ns << "\n"
         << "}\n";
  verify_output(output);
}

} // namespace ipmx::sender
