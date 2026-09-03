// relay protocol: Mac client <-> OrangePi daemon
//
// TCP control channel (port 50001) - JSON lines, length-prefixed:
//   -> {"type":"hello","proto":1,"name":"...","pin":...}
//   <- {"type":"welcome","video_w":..,"video_h":..,"video_fov":..,"audio_channels":..,"audio_rate":..}
//   -> {"type":"ctrl","buttons":..,"lx":..,"ly":..,"rx":..,"ry":..,"l2":..,"r2":..}
//      (sent at 250Hz; daemon translates to chiaki controller state)
//   -> {"type":"ping"} / <- {"type":"pong"}
//
// UDP video channel (port 50002) - raw H.265 NAL units:
//   [u8 magic=0xC1][u32 le counter][u32 le size][u16 le flags][payload]
//   flags: bit0 = config (SPS/PPS/etc), bit1 = keyframe
//
// UDP audio channel (port 50003) - opus frames:
//   [u8 magic=0xC2][u32 le counter][u16 le size][payload]
#ifndef RELAY_PROTO_H
#define RELAY_PROTO_H

#define RELAY_TCP_PORT 50001
#define RELAY_VIDEO_PORT 50002
#define RELAY_AUDIO_PORT 50003

#define RELAY_VIDEO_MAGIC 0xC1
#define RELAY_AUDIO_MAGIC 0xC2

#define RELAY_FLAG_CONFIG 0x1
#define RELAY_FLAG_KEYFRAME 0x2

#endif
