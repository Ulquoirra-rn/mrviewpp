/* Copyright (c) 2008-2026 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Covered Software is provided under this License on an "as is"
 * basis, without warranty of any kind, either expressed, implied, or
 * statutory, including, without limitation, warranties that the
 * Covered Software is free of defects, merchantable, fit for a
 * particular purpose or non-infringing.
 * See the Mozilla Public License v. 2.0 for more details.
 *
 * For more details, see http://www.mrtrix.org/.
 */

#include <cstring>

#include "exception.h"
#include "file/dicom/dicom_decode.h"

#ifdef MRTRIX_JPEG_SUPPORT
#include <csetjmp>
extern "C" {
#include <jpeglib.h>
}
#endif

#ifdef MRTRIX_JPEG2000_SUPPORT
#include <openjpeg.h>
#endif

namespace MR
{
  namespace File
  {
    namespace Dicom
    {

      namespace {
        // Write one sample value as `bytes` little-endian bytes into out[pos..].
        inline void put_le (vector<uint8_t>& out, size_t pos, uint32_t v, size_t bytes) {
          if (pos + bytes > out.size()) return;
          out[pos] = v & 0xFF;
          if (bytes >= 2) out[pos+1] = (v >> 8) & 0xFF;
          if (bytes >= 4) { out[pos+2] = (v>>16)&0xFF; out[pos+3] = (v>>24)&0xFF; }
        }
      }


#ifdef MRTRIX_JPEG_SUPPORT

      namespace {
        struct JPEGError { struct jpeg_error_mgr pub; std::jmp_buf setjmp_buffer; };
        void jpeg_error_throw (j_common_ptr cinfo) {
          std::longjmp (reinterpret_cast<JPEGError*> (cinfo->err)->setjmp_buffer, 1);
        }
      }

      vector<uint8_t> decode_jpeg (const vector<uint8_t>& in, size_t rows, size_t cols,
                                   size_t bits, size_t samples, bool /*is_signed*/)
      {
        const size_t bytes = (bits + 7) / 8;
        vector<uint8_t> out (rows * cols * samples * bytes, 0);
        if (in.empty())
          return out;

        jpeg_decompress_struct cinfo;
        JPEGError jerr;
        cinfo.err = jpeg_std_error (&jerr.pub);
        jerr.pub.error_exit = jpeg_error_throw;
        if (setjmp (jerr.setjmp_buffer)) {
          jpeg_destroy_decompress (&cinfo);
          throw Exception ("failed to decode JPEG-compressed DICOM frame");
        }

        jpeg_create_decompress (&cinfo);
        jpeg_mem_src (&cinfo, in.data(), in.size());
        jpeg_read_header (&cinfo, TRUE);
        jpeg_start_decompress (&cinfo);

        const size_t w = cinfo.output_width;
        const size_t h = cinfo.output_height;
        const size_t comp = cinfo.output_components;
        const int prec = cinfo.data_precision;
        const size_t per_row = w * comp;

        size_t y = 0;
        if (prec <= 8) {
          vector<JSAMPLE> row (per_row);
          JSAMPROW rp = row.data();
          while (cinfo.output_scanline < h) {
            jpeg_read_scanlines (&cinfo, &rp, 1);
            for (size_t i = 0; i != per_row; ++i)
              put_le (out, (y * per_row + i) * bytes, uint32_t (row[i]), bytes);
            ++y;
          }
        } else {
          // 9-16 bit samples: libjpeg-turbo returns 16-bit scanlines.
          vector<J16SAMPLE> row (per_row);
          J16SAMPROW rp = row.data();
          while (cinfo.output_scanline < h) {
            jpeg16_read_scanlines (&cinfo, &rp, 1);
            for (size_t i = 0; i != per_row; ++i)
              put_le (out, (y * per_row + i) * bytes, uint32_t (row[i]), bytes);
            ++y;
          }
        }

        jpeg_finish_decompress (&cinfo);
        jpeg_destroy_decompress (&cinfo);
        (void) w;
        return out;
      }

#else
      vector<uint8_t> decode_jpeg (const vector<uint8_t>&, size_t, size_t, size_t, size_t, bool)
      { throw Exception ("JPEG-compressed DICOM decoding is not available in this build"); }
#endif



#ifdef MRTRIX_JPEG2000_SUPPORT

      namespace {
        struct MemStream { const uint8_t* data; OPJ_SIZE_T size, pos; };
        OPJ_SIZE_T mem_read (void* buf, OPJ_SIZE_T n, void* p) {
          MemStream* m = reinterpret_cast<MemStream*> (p);
          const OPJ_SIZE_T rem = m->size - m->pos;
          if (!rem) return (OPJ_SIZE_T)-1;
          if (n > rem) n = rem;
          memcpy (buf, m->data + m->pos, n);
          m->pos += n;
          return n;
        }
        OPJ_OFF_T mem_skip (OPJ_OFF_T n, void* p) {
          MemStream* m = reinterpret_cast<MemStream*> (p);
          if (n < 0) n = 0;
          if ((OPJ_SIZE_T)n > m->size - m->pos) n = m->size - m->pos;
          m->pos += n;
          return n;
        }
        OPJ_BOOL mem_seek (OPJ_OFF_T n, void* p) {
          MemStream* m = reinterpret_cast<MemStream*> (p);
          if (n < 0 || (OPJ_SIZE_T)n > m->size) return OPJ_FALSE;
          m->pos = n;
          return OPJ_TRUE;
        }
        void opj_quiet (const char*, void*) { }
      }

      vector<uint8_t> decode_jpeg2000 (const vector<uint8_t>& in, size_t rows, size_t cols,
                                       size_t bits, size_t samples, bool /*is_signed*/)
      {
        const size_t bytes = (bits + 7) / 8;
        vector<uint8_t> out (rows * cols * samples * bytes, 0);
        if (in.empty())
          return out;

        // DICOM uses the raw J2K codestream (magic FF 4F FF 51); handle a JP2
        // wrapper too, just in case.
        OPJ_CODEC_FORMAT fmt = OPJ_CODEC_J2K;
        if (in.size() > 12 && in[0]==0x00 && in[1]==0x00 && in[2]==0x00 && in[3]==0x0C &&
            in[4]==0x6A && in[5]==0x50)
          fmt = OPJ_CODEC_JP2;

        MemStream ms { in.data(), (OPJ_SIZE_T) in.size(), 0 };
        opj_stream_t* stream = opj_stream_default_create (OPJ_TRUE);
        opj_stream_set_user_data (stream, &ms, nullptr);
        opj_stream_set_user_data_length (stream, ms.size);
        opj_stream_set_read_function (stream, mem_read);
        opj_stream_set_skip_function (stream, mem_skip);
        opj_stream_set_seek_function (stream, mem_seek);

        opj_codec_t* codec = opj_create_decompress (fmt);
        opj_set_warning_handler (codec, opj_quiet, nullptr);
        opj_set_info_handler (codec, opj_quiet, nullptr);
        opj_dparameters_t params;
        opj_set_default_decoder_parameters (&params);
        opj_setup_decoder (codec, &params);

        opj_image_t* image = nullptr;
        if (!opj_read_header (stream, codec, &image) ||
            !opj_decode (codec, stream, image) ||
            !opj_end_decompress (codec, stream)) {
          if (image) opj_image_destroy (image);
          opj_destroy_codec (codec);
          opj_stream_destroy (stream);
          throw Exception ("failed to decode JPEG 2000-compressed DICOM frame");
        }

        const size_t ncomp = image->numcomps;
        const size_t w = image->comps[0].w;
        const size_t h = image->comps[0].h;
        for (size_t c = 0; c < ncomp; ++c) {
          const OPJ_INT32* d = image->comps[c].data;
          if (!d) continue;
          for (size_t i = 0; i < w * h; ++i)
            put_le (out, (i * samples + c) * bytes, uint32_t (d[i]), bytes);
        }

        opj_image_destroy (image);
        opj_destroy_codec (codec);
        opj_stream_destroy (stream);
        (void) rows; (void) cols;
        return out;
      }

#else
      vector<uint8_t> decode_jpeg2000 (const vector<uint8_t>&, size_t, size_t, size_t, size_t, bool)
      { throw Exception ("JPEG 2000-compressed DICOM decoding is not available in this build"); }
#endif

    }
  }
}
