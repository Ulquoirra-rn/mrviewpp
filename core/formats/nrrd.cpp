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

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include "header.h"
#include "datatype.h"
#include "file/path.h"
#include "image_io/default.h"
#include "image_io/nrrd.h"
#include "formats/list.h"

namespace MR
{
  namespace Formats
  {

    namespace {

      inline std::string lowercase (std::string s) {
        std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return std::tolower (c); });
        return s;
      }

      inline std::string strip_ws (const std::string& s) {
        const size_t a = s.find_first_not_of (" \t\r\n");
        if (a == std::string::npos) return "";
        const size_t b = s.find_last_not_of (" \t\r\n");
        return s.substr (a, b - a + 1);
      }

      // split on whitespace
      inline vector<std::string> tokenize (const std::string& s) {
        vector<std::string> out;
        std::istringstream iss (s);
        std::string t;
        while (iss >> t)
          out.push_back (t);
        return out;
      }

      // parse a NRRD vector token "(x,y,z)" into its components
      inline vector<default_type> parse_vector (std::string t) {
        vector<default_type> v;
        const size_t open = t.find ('(');
        const size_t close = t.find (')');
        if (open == std::string::npos || close == std::string::npos || close < open)
          return v;
        t = t.substr (open + 1, close - open - 1);
        std::replace (t.begin(), t.end(), ',', ' ');
        std::istringstream iss (t);
        default_type x;
        while (iss >> x)
          v.push_back (x);
        return v;
      }

      // map a NRRD "type" field to an MRtrix DataType code, applying endianness
      // for multi-byte types ('little' selects little-endian).
      uint8_t nrrd_datatype (std::string type, const bool little) {
        type = lowercase (strip_ws (type));
        if (type == "signed char" || type == "int8" || type == "int8_t")
          return DataType::Int8;
        if (type == "uchar" || type == "unsigned char" || type == "uint8" || type == "uint8_t")
          return DataType::UInt8;
        if (type == "short" || type == "short int" || type == "signed short" ||
            type == "signed short int" || type == "int16" || type == "int16_t")
          return little ? DataType::Int16LE : DataType::Int16BE;
        if (type == "ushort" || type == "unsigned short" || type == "unsigned short int" ||
            type == "uint16" || type == "uint16_t")
          return little ? DataType::UInt16LE : DataType::UInt16BE;
        if (type == "int" || type == "signed int" || type == "int32" || type == "int32_t")
          return little ? DataType::Int32LE : DataType::Int32BE;
        if (type == "uint" || type == "unsigned int" || type == "uint32" || type == "uint32_t")
          return little ? DataType::UInt32LE : DataType::UInt32BE;
        if (type == "longlong" || type == "long long" || type == "long long int" ||
            type == "signed long long" || type == "signed long long int" ||
            type == "int64" || type == "int64_t")
          return little ? DataType::Int64LE : DataType::Int64BE;
        if (type == "ulonglong" || type == "unsigned long long" || type == "unsigned long long int" ||
            type == "uint64" || type == "uint64_t")
          return little ? DataType::UInt64LE : DataType::UInt64BE;
        if (type == "float")
          return little ? DataType::Float32LE : DataType::Float32BE;
        if (type == "double")
          return little ? DataType::Float64LE : DataType::Float64BE;
        throw Exception ("unsupported NRRD data type \"" + type + "\"");
      }

    }




    std::unique_ptr<ImageIO::Base> NRRD::read (Header& H) const
    {
      if (!Path::has_suffix (H.name(), ".nrrd") && !Path::has_suffix (H.name(), ".nhdr"))
        return std::unique_ptr<ImageIO::Base>();

      std::ifstream in (H.name(), std::ios_base::binary);
      if (!in)
        throw Exception ("Error opening NRRD file \"" + H.name() + "\"");

      std::string line;
      std::getline (in, line);
      if (line.compare (0, 4, "NRRD") != 0)
        return std::unique_ptr<ImageIO::Base>();

      std::map<std::string, std::string> field;
      while (std::getline (in, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty())
          break;                       // blank line terminates the header
        if (line[0] == '#')
          continue;                    // comment
        const size_t colon = line.find (':');
        if (colon == std::string::npos)
          continue;
        // "key:=value" is a generic key/value pair (metadata); "field: value"
        // is a NRRD field specification. Only the latter is interpreted here.
        if (colon + 1 < line.size() && line[colon + 1] == '=')
          continue;
        field[lowercase (strip_ws (line.substr (0, colon)))] = strip_ws (line.substr (colon + 1));
      }
      const int64_t attached_data_offset = in.tellg();
      in.close();

      auto get = [&] (const std::string& k) -> std::string {
        auto it = field.find (k);
        return it == field.end() ? std::string() : it->second;
      };

      if (get ("dimension").empty() || get ("sizes").empty() || get ("type").empty())
        throw Exception ("malformed NRRD file \"" + H.name() + "\" (missing dimension/sizes/type)");

      const size_t ndim = std::stoul (get ("dimension"));
      const vector<std::string> size_tokens = tokenize (get ("sizes"));
      if (size_tokens.size() != ndim)
        throw Exception ("NRRD file \"" + H.name() + "\": number of sizes does not match dimension");

      H.ndim() = ndim;
      for (size_t i = 0; i != ndim; ++i) {
        H.size (i) = std::stoll (size_tokens[i]);
        H.stride (i) = i + 1;          // NRRD stores the first axis fastest-varying
      }

      // data type + endianness
      bool little = true;              // default when unspecified (most NRRD writers)
      const std::string endian = lowercase (get ("endian"));
      if (endian == "big")
        little = false;
      H.datatype() = DataType (nrrd_datatype (get ("type"), little));
      H.reset_intensity_scaling();

      // spatial transform from "space directions" + "space origin"
      transform_type& M (H.transform());
      M.setIdentity();
      const vector<std::string> dir_tokens = tokenize (get ("space directions"));
      const vector<std::string> spacing_tokens = tokenize (get ("spacings"));
      for (size_t i = 0; i != std::min<size_t> (3, ndim); ++i) {
        if (i < dir_tokens.size() && lowercase (dir_tokens[i]) != "none") {
          const vector<default_type> v = parse_vector (dir_tokens[i]);
          if (v.size() >= 3) {
            const default_type norm = std::sqrt (v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            H.spacing (i) = norm;
            if (norm > 0.0) {
              M(0,i) = v[0] / norm;
              M(1,i) = v[1] / norm;
              M(2,i) = v[2] / norm;
            }
          }
        } else if (i < spacing_tokens.size() && lowercase (spacing_tokens[i]) != "nan") {
          H.spacing (i) = std::stod (spacing_tokens[i]);
        }
      }
      const vector<default_type> origin = parse_vector (get ("space origin"));
      if (origin.size() >= 3)
        for (size_t r = 0; r != 3; ++r)
          M(r,3) = origin[r];

      // NRRD anatomical spaces other than RAS need axis flips to reach MRtrix's
      // scanner (RAS) convention. Negate the relevant rows of the transform.
      const std::string space = lowercase (get ("space"));
      const bool flip_x = (space.compare (0, 4, "left") == 0);                 // L** -> R**
      const bool flip_y = (space.find ("posterior") != std::string::npos);     // *P* -> *A*
      if (flip_x) M.matrix().row(0) *= -1.0;
      if (flip_y) M.matrix().row(1) *= -1.0;

      // locate the data block
      const std::string encoding = lowercase (get ("encoding"));
      int64_t byteskip = 0;
      if (!get ("byte skip").empty())      byteskip = std::stoll (get ("byte skip"));
      else if (!get ("byteskip").empty())  byteskip = std::stoll (get ("byteskip"));

      std::string datafile = H.name();
      int64_t offset = attached_data_offset;
      const std::string detached = !get ("data file").empty() ? get ("data file") : get ("datafile");
      if (!detached.empty()) {
        if (detached == "LIST" || detached.find ('%') != std::string::npos)
          throw Exception ("multi-file (LIST / printf) NRRD data is not yet supported: \"" + H.name() + "\"");
        const bool absolute = (!detached.empty() && detached[0] == '/');
        datafile = absolute ? detached : Path::join (Path::dirname (H.name()), detached);
        offset = std::max<int64_t> (byteskip, 0);
      } else if (byteskip > 0) {
        offset += byteskip;
      }

      if (encoding == "raw") {
        if (byteskip < 0) {                // raw "-1": data sits at end of file
          int64_t fsize = 0;
          { std::ifstream f (datafile, std::ios_base::binary | std::ios_base::ate); if (f) fsize = f.tellg(); }
          offset = fsize - static_cast<int64_t> (footprint (H));
        }
        std::unique_ptr<ImageIO::Base> io (new ImageIO::Default (H));
        io->files.push_back (File::Entry (datafile, offset));
        return io;
      }

      if (encoding == "gzip" || encoding == "gz") {
        std::unique_ptr<ImageIO::Base> io (new ImageIO::NRRD (H));
        io->files.push_back (File::Entry (datafile, offset));
        return io;
      }

      throw Exception ("NRRD encoding \"" + encoding + "\" is not yet supported for \"" + H.name() +
                       "\" (supported: raw, gzip)");
    }





    bool NRRD::check (Header&, size_t) const
    {
      // Writing of NRRD images is not yet implemented; this handler is
      // read-only, so it never advertises itself for image creation.
      return false;
    }





    std::unique_ptr<ImageIO::Base> NRRD::create (Header& H) const
    {
      throw Exception ("writing of NRRD images is not yet supported (\"" + H.name() + "\")");
    }

  }
}
