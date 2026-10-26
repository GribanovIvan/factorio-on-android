/* DXT/BC -> ETC2/EAC transcoding for drivers without S3TC/RGTC (rev-3).
 *
 * Standalone: no GL headers, no glcompat state, allocates with malloc().
 * Host-side unit testable (tools/etc2_test.c compiles this file directly).
 *
 * Format mapping (all GLES 3.0 core, hardware-decoded on Mali):
 *   DXT1  (0x83F0) -> ETC2 RGB8        (0x9274, 4 bpp)
 *   DXT3  (0x83F2) -> ETC2 RGBA8 EAC   (0x9278, 16 bpp)
 *   DXT5  (0x83F3) -> ETC2 RGBA8 EAC   (0x9278, 16 bpp)
 *   BC4   (0x8DBB) -> R8   uncompressed (decode, 8 bpp)
 *   BC5   (0x8DBD) -> RG8  uncompressed (decode, 16 bpp)
 *
 * DXT1A (0x83F1, 1-bit alpha) has not been observed from the game and is not
 * mapped here; callers fall back to the rev-2 RGBA8 decode for it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL enums, spelled out to keep this header standalone. */
#define FSGL_ETC2_RGB8 0x9274u
#define FSGL_ETC2_RGBA8 0x9278u
#define FSGL_R8 0x8229u
#define FSGL_RG8 0x822Bu

/* Decode an ETC2 RGB8 (0x9274) or ETC2 RGBA8 (0x9278) image produced by
 * this transcoder back to tightly packed RGBA8 (w*h*4 bytes).  Used for
 * glGetTexImage readback on textures the driver stores as ETC2 (those
 * cannot be attached to an FBO).  Handles differential and individual
 * colour blocks plus EAC alpha.  Returns 0 on success, -1 on bad input. */
int fsgl_etc2_decode_rgba(uint32_t etc2_fmt, int w, int h, const void *data,
                          size_t size, unsigned char *out_rgba);

/* Encode a tightly packed RGBA8 image (w*h*4 bytes) into ETC2 RGB8 or
 * ETC2 RGBA8 block data.  out must hold fsgl_etc2_dest_size(fmt, w, h)
 * bytes.  Used by the glGenerateMipmap emulation for transcoded
 * textures.  Returns 0 on success, -1 on bad input. */
int fsgl_etc2_encode_rgba(uint32_t dest_fmt, int w, int h, const void *rgba,
                          unsigned char *out);

/* 2x2 alpha-weighted box filter (straight alpha): one mipmap level.
 * src is sw*sh*4 bytes, dst dw*dh*4 (dw,dh <= ceil(sw/2),ceil(sh/2)). */
void fsgl_etc2_box_filter_rgba(const unsigned char *src, int sw, int sh,
                               unsigned char *dst, int dw, int dh);

/* Destination format for a source DXT/BC format, or 0 when unmappable. */
uint32_t fsgl_etc2_dest_format(uint32_t dxt_fmt);

/* Destination image size in bytes for a full image (not sub-rect aware;
 * callers compute sub-rect sizes from the same block arithmetic). */
size_t fsgl_etc2_dest_size(uint32_t dest_fmt, int w, int h);

/* Bytes per destination block: 8 for ETC2 RGB, 16 for RGBA (EAC + ETC2),
 * 0 for uncompressed destinations. */
unsigned fsgl_etc2_dest_block_bytes(uint32_t dest_fmt);

/* Transcode one image or sub-rect of DXT/BC data.
 *
 *   dxt_fmt   source format (0x83F0/0x83F2/0x83F3/0x8DBB/0x8DBD)
 *   w, h      sub-rect size in pixels (block-aligned coverage is implied)
 *   data      source bytes, ceil(w/4)*ceil(h/4) source blocks
 *   image_size sanity-check size; 0 disables the check
 *   out_fmt   set to the destination format on success
 *   out_size  set to the destination byte count on success
 *
 * Returns malloc()ed destination bytes (caller frees) or NULL.  For
 * uncompressed destinations the result is tightly packed w*h (R8) or
 * w*h*2 (RG8) bytes.
 */
unsigned char *fsgl_transcode_to_etc2(uint32_t dxt_fmt, int w, int h,
                                      const void *data, size_t image_size,
                                      uint32_t *out_fmt, size_t *out_size);

#ifdef __cplusplus
}
#endif
