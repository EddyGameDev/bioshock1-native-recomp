#include "xenos_hlsl.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace {

using namespace bs1::renderer::XenosHlsl;

const char* const kComponents = "xyzw";

std::string Format(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  return buffer;
}

struct Translator {
  const std::vector<uint32_t>& words;
  const bool pixel;
  Translation out;
  std::string body;
  std::set<uint32_t> vertexSlots;
  std::map<uint32_t, uint32_t> vertexStrides;
  std::set<uint32_t> textureSlots;
  std::set<uint32_t> exportsUsed;
  int indent = 1;

  std::set<uint32_t> constantsRead;
  bool relativeConstants = false;
  uint32_t lastFullVertexSlot = 0;

  Translator(const std::vector<uint32_t>& w, bool p) : words(w), pixel(p) {}

  std::string Constant(uint32_t index, bool relative) {
    if (relative) {
      relativeConstants = true;
      return Format("c[(a0 + %u) & 255]", index & 0xFF);
    }
    constantsRead.insert(index & 0xFF);
    return Format("c[%u]", index & 0xFF);
  }

  void PackConstants() {
    if (relativeConstants || constantsRead.empty()) return;
    uint32_t position[256] = {};
    for (uint32_t index : constantsRead) {
      position[index] = static_cast<uint32_t>(out.constantMap.size());
      out.constantMap.push_back(static_cast<uint16_t>(index));
    }
    std::string packed;
    packed.reserve(body.size());
    for (size_t i = 0; i < body.size();) {
      const bool word =
          i > 0 && (std::isalnum(static_cast<unsigned char>(body[i - 1])) || body[i - 1] == '_');
      if (!word && body.compare(i, 2, "c[") == 0 &&
          std::isdigit(static_cast<unsigned char>(body[i + 2]))) {
        size_t end = i + 2;
        while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end]))) ++end;
        if (end < body.size() && body[end] == ']') {
          const uint32_t index =
              static_cast<uint32_t>(std::strtoul(body.c_str() + i + 2, nullptr, 10)) & 0xFF;
          packed += Format("c[%u]", position[index]);
          i = end + 1;
          continue;
        }
      }
      packed += body[i];
      ++i;
    }
    body.swap(packed);
  }

  void Line(const std::string& text) {
    body.append(static_cast<size_t>(indent) * 4, ' ');
    body += text;
    body += '\n';
  }

  std::string Source(uint32_t reg, bool temporary, uint32_t swizzle, bool negate, bool absolute,
                     bool relative) {
    std::string base;
    if (temporary) {
      base = (reg & 0x40) ? Format("r[(%u + aL) & 31]", reg & 0x1F) : Format("r[%u]", reg & 0x1F);
    } else {
      base = Constant(reg, relative);
    }
    std::string swizzled = base + ".";
    for (uint32_t i = 0; i < 4; ++i) {
      swizzled += kComponents[(i + ((swizzle >> (i * 2)) & 3)) & 3];
    }
    if (absolute) swizzled = "abs(" + swizzled + ")";
    if (negate) swizzled = "(-" + swizzled + ")";
    return swizzled;
  }

  void ScalarPair(uint32_t reg, bool temporary, uint32_t swizzle, bool negate, bool absolute,
                  bool relative, bool vectorTakesThree, std::string& a, std::string& b,
                  std::string& single) {
    std::string base;
    if (temporary) {
      base = (reg & 0x40) ? Format("r[(%u + aL) & 31]", reg & 0x1F) : Format("r[%u]", reg & 0x1F);
    } else {
      base = Constant(reg, relative);
    }
    const uint32_t laneA = vectorTakesThree ? 2 : 3;
    const uint32_t laneB = vectorTakesThree ? 3 : 0;
    a = base + "." + kComponents[(laneA + ((swizzle >> (laneA * 2)) & 3)) & 3];
    b = base + "." + kComponents[(laneB + ((swizzle >> (laneB * 2)) & 3)) & 3];
    single = base + "." + kComponents[(3 + ((swizzle >> 6) & 3)) & 3];
    if (absolute) {
      single = "abs(" + single + ")";
      a = "abs(" + a + ")";
      b = "abs(" + b + ")";
    }
    if (negate) {
      single = "(-" + single + ")";
      a = "(-" + a + ")";
      b = "(-" + b + ")";
    }
  }

  std::string Mask(uint32_t mask) {
    std::string text;
    for (uint32_t i = 0; i < 4; ++i) {
      if (mask & (1u << i)) text += kComponents[i];
    }
    return text;
  }

  std::string Destination(uint32_t reg, bool exported) {
    if (!exported) return Format("r[%u]", reg & 0x1F);
    exportsUsed.insert(reg);
    if (pixel) {
      if (reg < 4) return Format("oC%u", reg);
      if (reg == 61) {
        out.writesDepth = true;
        return "oDepth4";
      }
      out.problem = Format("pixel export %u", reg);
      return "oUnused";
    }
    if (reg < 16) return Format("o%u", reg);
    if (reg == 62) return "oPos";
    if (reg == 63) return "oPointSize";
    out.problem = Format("vertex export %u", reg);
    return "oUnused";
  }

  void Alu(uint32_t d0, uint32_t d1, uint32_t d2) {
    const uint32_t vectorDest = d0 & 0x3F;
    const bool absoluteConstants = ((d0 >> 7) & 1) != 0;
    const uint32_t scalarDest = (d0 >> 8) & 0x3F;
    const bool exported = ((d0 >> 15) & 1) != 0;
    const uint32_t vectorMask = (d0 >> 16) & 0xF;
    const uint32_t scalarMask = (d0 >> 20) & 0xF;
    const bool vectorClamp = ((d0 >> 24) & 1) != 0;
    const bool scalarClamp = ((d0 >> 25) & 1) != 0;
    const uint32_t scalarOpcode = (d0 >> 26) & 0x3F;

    const uint32_t swizzle3 = d1 & 0xFF;
    const uint32_t swizzle2 = (d1 >> 8) & 0xFF;
    const uint32_t swizzle1 = (d1 >> 16) & 0xFF;
    const bool negate3 = ((d1 >> 24) & 1) != 0;
    const bool negate2 = ((d1 >> 25) & 1) != 0;
    const bool negate1 = ((d1 >> 26) & 1) != 0;
    const bool predicateCondition = ((d1 >> 27) & 1) != 0;
    const bool isPredicated = ((d1 >> 28) & 1) != 0;
    const bool relative0 = ((d1 >> 31) & 1) != 0;
    const bool relative1 = ((d1 >> 30) & 1) != 0;

    const uint32_t reg3 = d2 & 0xFF;
    const uint32_t reg2 = (d2 >> 8) & 0xFF;
    const uint32_t reg1 = (d2 >> 16) & 0xFF;
    const uint32_t vectorOpcode = (d2 >> 24) & 0x1F;
    const bool temp3 = ((d2 >> 29) & 1) != 0;
    const bool temp2 = ((d2 >> 30) & 1) != 0;
    const bool temp1 = ((d2 >> 31) & 1) != 0;

    const bool rel1 = !temp1 && relative0;
    const bool rel2 = !temp2 && (temp1 ? relative0 : relative1);
    const bool rel3 = !temp3 && ((temp1 && temp2) ? relative0 : relative1);

    const std::string a = Source(reg1, temp1, swizzle1, negate1,
                                 temp1 ? (reg1 & 0x80) != 0 : absoluteConstants, rel1);
    const std::string b = Source(reg2, temp2, swizzle2, negate2,
                                 temp2 ? (reg2 & 0x80) != 0 : absoluteConstants, rel2);
    const std::string c = Source(reg3, temp3, swizzle3, negate3,
                                 temp3 ? (reg3 & 0x80) != 0 : absoluteConstants, rel3);
    std::string sa, sb, s1;
    const bool vectorTakesThree =
        vectorOpcode == 11 || (vectorOpcode >= 12 && vectorOpcode <= 14) || vectorOpcode == 17;
    ScalarPair(reg3, temp3, swizzle3, negate3, temp3 ? (reg3 & 0x80) != 0 : absoluteConstants, rel3,
               vectorTakesThree, sa, sb, s1);

    Line("{");
    ++indent;
    if (isPredicated) {
      Line(Format("if (p0 == %s) {", predicateCondition ? "true" : "false"));
      ++indent;
    }

    std::string vector;
    bool kill = false;
    switch (vectorOpcode) {
      case 0:  vector = Format("(%s + %s)", a.c_str(), b.c_str()); break;
      case 1:  vector = Format("mulL(%s, %s)", a.c_str(), b.c_str()); break;
      case 2:  vector = Format("max(%s, %s)", a.c_str(), b.c_str()); break;
      case 3:  vector = Format("min(%s, %s)", a.c_str(), b.c_str()); break;
      case 4:  vector = Format("select4(%s == %s)", a.c_str(), b.c_str()); break;
      case 5:  vector = Format("select4(%s > %s)", a.c_str(), b.c_str()); break;
      case 6:  vector = Format("select4(%s >= %s)", a.c_str(), b.c_str()); break;
      case 7:  vector = Format("select4(%s != %s)", a.c_str(), b.c_str()); break;
      case 8:  vector = Format("frac(%s)", a.c_str()); break;
      case 9:  vector = Format("trunc(%s)", a.c_str()); break;
      case 10: vector = Format("floor(%s)", a.c_str()); break;
      case 11: vector = Format("(mulL(%s, %s) + %s)", a.c_str(), b.c_str(), c.c_str()); break;
      case 12: vector = Format("blend4(select4(%s == 0.0), %s, %s)", a.c_str(), b.c_str(), c.c_str()); break;
      case 13: vector = Format("blend4(select4(%s >= 0.0), %s, %s)", a.c_str(), b.c_str(), c.c_str()); break;
      case 14: vector = Format("blend4(select4(%s > 0.0), %s, %s)", a.c_str(), b.c_str(), c.c_str()); break;
      case 15: vector = Format("dot(mulL(%s, %s), float4(1.0, 1.0, 1.0, 1.0)).xxxx", a.c_str(), b.c_str()); break;
      case 16: vector = Format("dot(mulL(%s, %s).xyz, float3(1.0, 1.0, 1.0)).xxxx", a.c_str(), b.c_str()); break;
      case 17: vector = Format("(dot(mulL(%s, %s).xy, float2(1.0, 1.0)) + (%s).x).xxxx", a.c_str(), b.c_str(), c.c_str()); break;
      case 18: vector = Format("cubeMap(%s, %s)", a.c_str(), b.c_str()); break;
      case 19: vector = Format("max4(%s).xxxx", a.c_str()); break;
      case 20:
        vector = Format("((%s).w == 0.0 ? 0.0 : (%s).x + 1.0).xxxx", a.c_str(), b.c_str());
        Line(Format("p0 = ((%s).w == 0.0) && ((%s).x == 0.0);", a.c_str(), b.c_str()));
        break;
      case 21:
        vector = Format("((%s).w == 0.0 ? 0.0 : (%s).x + 1.0).xxxx", a.c_str(), b.c_str());
        Line(Format("p0 = ((%s).w == 0.0) && ((%s).x != 0.0);", a.c_str(), b.c_str()));
        break;
      case 22:
        vector = Format("((%s).w == 0.0 ? 0.0 : (%s).x + 1.0).xxxx", a.c_str(), b.c_str());
        Line(Format("p0 = ((%s).w == 0.0) && ((%s).x > 0.0);", a.c_str(), b.c_str()));
        break;
      case 23:
        vector = Format("((%s).w == 0.0 ? 0.0 : (%s).x + 1.0).xxxx", a.c_str(), b.c_str());
        Line(Format("p0 = ((%s).w == 0.0) && ((%s).x >= 0.0);", a.c_str(), b.c_str()));
        break;
      case 24: vector = Format("select4(%s == %s)", a.c_str(), b.c_str()); kill = true; break;
      case 25: vector = Format("select4(%s > %s)", a.c_str(), b.c_str()); kill = true; break;
      case 26: vector = Format("select4(%s >= %s)", a.c_str(), b.c_str()); kill = true; break;
      case 27: vector = Format("select4(%s != %s)", a.c_str(), b.c_str()); kill = true; break;
      case 28: vector = Format("float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a.c_str(), b.c_str(), a.c_str(), b.c_str()); break;
      case 29:
        vector = Format("max(%s, %s)", a.c_str(), b.c_str());
        Line(Format("a0 = clamp((int)floor((%s).w + 0.5), -256, 255);", a.c_str()));
        break;
      default:
        out.problem = Format("vector opcode %u", vectorOpcode);
        vector = "float4(0.0, 0.0, 0.0, 0.0)";
        break;
    }

    if (kill && pixel) {
      out.usesKill = true;
      Line(Format("if (any(%s)) discard;", vector.c_str()));
    }

    std::string vectorDestination;
    if (vectorMask != 0) {
      if (vectorClamp) vector = "saturate(" + vector + ")";
      vectorDestination = Destination(vectorDest, exported);
      Line(Format("float4 vectorValue = (%s);", vector.c_str()));
    }

    std::string scalar;
    bool scalarKill = false;
    switch (scalarOpcode) {
      case 0:  scalar = Format("(%s + %s)", sa.c_str(), sb.c_str()); break;
      case 1:  scalar = Format("(%s + ps)", s1.c_str()); break;
      case 2:  scalar = Format("mulL1(%s, %s)", sa.c_str(), sb.c_str()); break;
      case 3:  scalar = Format("mulL1(%s, ps)", s1.c_str()); break;
      case 4:
        scalar = Format("((ps == -3.402823466e38 || isnan(ps) || isnan(%s) || %s <= 0.0) ? -3.402823466e38 : %s * ps)",
                        sb.c_str(), sb.c_str(), sa.c_str());
        break;
      case 5:  scalar = Format("max(%s, %s)", sa.c_str(), sb.c_str()); break;
      case 6:  scalar = Format("min(%s, %s)", sa.c_str(), sb.c_str()); break;
      case 7:  scalar = Format("(%s == 0.0 ? 1.0 : 0.0)", s1.c_str()); break;
      case 8:  scalar = Format("(%s > 0.0 ? 1.0 : 0.0)", s1.c_str()); break;
      case 9:  scalar = Format("(%s >= 0.0 ? 1.0 : 0.0)", s1.c_str()); break;
      case 10: scalar = Format("(%s != 0.0 ? 1.0 : 0.0)", s1.c_str()); break;
      case 11: scalar = Format("frac(%s)", s1.c_str()); break;
      case 12: scalar = Format("trunc(%s)", s1.c_str()); break;
      case 13: scalar = Format("floor(%s)", s1.c_str()); break;
      case 14: scalar = Format("exp2(%s)", s1.c_str()); break;
      case 15:
      case 16: scalar = Format("(%s > 0.0 ? log2(%s) : -3.402823466e38)", s1.c_str(), s1.c_str()); break;
      case 17: scalar = Format("(%s == 0.0 ? 3.402823466e38 : 1.0 / %s)", s1.c_str(), s1.c_str()); break;
      case 18: scalar = Format("(%s == 0.0 ? 0.0 : 1.0 / %s)", s1.c_str(), s1.c_str()); break;
      case 19: scalar = Format("(1.0 / %s)", s1.c_str()); break;
      case 20: scalar = Format("(%s <= 0.0 ? 3.402823466e38 : rsqrt(%s))", s1.c_str(), s1.c_str()); break;
      case 21: scalar = Format("(%s <= 0.0 ? 0.0 : rsqrt(%s))", s1.c_str(), s1.c_str()); break;
      case 22: scalar = Format("rsqrt(%s)", s1.c_str()); break;
      case 23:
        scalar = Format("max(%s, %s)", sa.c_str(), sb.c_str());
        Line(Format("a0 = clamp((int)floor(%s + 0.5), -256, 255);", sa.c_str()));
        break;
      case 24:
        scalar = Format("max(%s, %s)", sa.c_str(), sb.c_str());
        Line(Format("a0 = clamp((int)floor(%s), -256, 255);", sa.c_str()));
        break;
      case 25: scalar = Format("(%s - %s)", sa.c_str(), sb.c_str()); break;
      case 26: scalar = Format("(%s - ps)", s1.c_str()); break;
      case 27: Line(Format("p0 = (%s == 0.0);", s1.c_str())); scalar = "(p0 ? 0.0 : 1.0)"; break;
      case 28: Line(Format("p0 = (%s != 0.0);", s1.c_str())); scalar = "(p0 ? 0.0 : 1.0)"; break;
      case 29: Line(Format("p0 = (%s > 0.0);", s1.c_str())); scalar = "(p0 ? 0.0 : 1.0)"; break;
      case 30: Line(Format("p0 = (%s >= 0.0);", s1.c_str())); scalar = "(p0 ? 0.0 : 1.0)"; break;
      case 31:
        Line(Format("p0 = (%s == 1.0);", s1.c_str()));
        scalar = Format("(p0 ? 0.0 : blend1(%s == 0.0, 1.0, %s))", s1.c_str(), s1.c_str());
        break;
      case 32:
        Line(Format("p0 = (%s - 1.0 <= 0.0);", s1.c_str()));
        scalar = Format("(p0 ? 0.0 : %s - 1.0)", s1.c_str());
        break;
      case 33: Line("p0 = false;"); scalar = "3.402823466e38"; break;
      case 34: Line(Format("p0 = (%s == 0.0);", s1.c_str())); scalar = s1; break;
      case 35: scalar = Format("(%s == 0.0 ? 1.0 : 0.0)", s1.c_str()); scalarKill = true; break;
      case 36: scalar = Format("(%s > 0.0 ? 1.0 : 0.0)", s1.c_str()); scalarKill = true; break;
      case 37: scalar = Format("(%s >= 0.0 ? 1.0 : 0.0)", s1.c_str()); scalarKill = true; break;
      case 38: scalar = Format("(%s != 0.0 ? 1.0 : 0.0)", s1.c_str()); scalarKill = true; break;
      case 39: scalar = Format("(%s == 1.0 ? 1.0 : 0.0)", s1.c_str()); scalarKill = true; break;
      case 40: scalar = Format("sqrt(%s)", s1.c_str()); break;
      case 42:
      case 43:
      case 44:
      case 45:
      case 46:
      case 47: {
        const uint32_t temporary =
            (scalarOpcode & 1) | (static_cast<uint32_t>(temp3) << 1) | (swizzle3 & 0x3C);
        std::string constant = Constant(reg3, rel3);
        constant += ".";
        constant += kComponents[(3 + ((swizzle3 >> 6) & 3)) & 3];
        std::string other = Format("r[%u].", temporary & 0x1F);
        other += kComponents[swizzle3 & 3];
        if (absoluteConstants) {
          constant = "abs(" + constant + ")";
          other = "abs(" + other + ")";
        }
        if (negate3) {
          constant = "(-" + constant + ")";
          other = "(-" + other + ")";
        }
        const char* op = scalarOpcode < 44 ? "*" : scalarOpcode < 46 ? "+" : "-";
        if (scalarOpcode < 44) {
          scalar = Format("mulL1(%s, %s)", constant.c_str(), other.c_str());
        } else {
          scalar = Format("(%s %s %s)", constant.c_str(), op, other.c_str());
        }
        break;
      }
      case 48: scalar = Format("sin(%s)", s1.c_str()); break;
      case 49: scalar = Format("cos(%s)", s1.c_str()); break;
      case 50: scalar = "ps"; break;
      default:
        out.problem = Format("scalar opcode %u", scalarOpcode);
        scalar = "0.0";
        break;
    }

    if (scalarKill && pixel) {
      out.usesKill = true;
      Line(Format("if (%s != 0.0) discard;", scalar.c_str()));
    }
    if (scalarClamp) scalar = "saturate(" + scalar + ")";
    Line(Format("ps = %s;", scalar.c_str()));

    if (vectorMask != 0) {
      Line(Format("%s.%s = vectorValue.%s;", vectorDestination.c_str(), Mask(vectorMask).c_str(),
                  Mask(vectorMask).c_str()));
    }
    if (scalarMask != 0) {
      const std::string destination = Destination(exported ? vectorDest : scalarDest, exported);
      Line(Format("%s.%s = ps.%s;", destination.c_str(), Mask(scalarMask).c_str(),
                  std::string(Mask(scalarMask).size(), 'x').c_str()));
    }

    if (isPredicated) {
      --indent;
      Line("}");
    }
    --indent;
    Line("}");
  }

  void FetchWrite(uint32_t destReg, uint32_t swizzle, const std::string& fetched) {
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t selector = (swizzle >> (i * 3)) & 7;
      std::string value;
      if (selector < 4) {
        value = fetched + "." + kComponents[selector];
      } else if (selector == 4 || selector == 6) {
        value = "0.0";
      } else if (selector == 5) {
        value = "1.0";
      } else {
        continue;
      }
      Line(Format("r[%u].%c = %s;", destReg & 0x1F, kComponents[i], value.c_str()));
    }
  }

  void VertexFetchInstruction(uint32_t d0, uint32_t d1, uint32_t d2) {
    const uint32_t srcReg = (d0 >> 5) & 0x3F;
    const uint32_t dstReg = (d0 >> 12) & 0x3F;
    const uint32_t rawSlot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 3);
    const uint32_t srcSwizzle = (d0 >> 30) & 3;
    const uint32_t dstSwizzle = d1 & 0xFFF;
    const bool isSigned = ((d1 >> 12) & 1) != 0;
    const bool whole = ((d1 >> 13) & 1) != 0;
    const bool rounded = ((d1 >> 15) & 1) != 0;
    const uint32_t format = (d1 >> 16) & 0x3F;
    int exponent = static_cast<int>((d1 >> 24) & 0x3F);
    if (exponent >= 32) exponent -= 64;
    const bool mini = ((d1 >> 30) & 1) != 0;
    const bool isPredicated = ((d1 >> 31) & 1) != 0;
    const uint32_t stride = d2 & 0xFF;
    int32_t offset = static_cast<int32_t>((d2 >> 8) & 0x7FFFFFu);
    if (offset >= 0x400000) offset -= 0x800000;
    const bool predicateCondition = ((d2 >> 31) & 1) != 0;

    const uint32_t slot = mini ? lastFullVertexSlot : rawSlot;
    if (!mini) lastFullVertexSlot = rawSlot;

    vertexSlots.insert(slot);
    if (!mini && stride != 0 && vertexStrides.count(slot) == 0) vertexStrides[slot] = stride;

    Line("{");
    ++indent;
    if (isPredicated) {
      Line(Format("if (p0 == %s) {", predicateCondition ? "true" : "false"));
      ++indent;
    }

    if (!mini) {
      Line(Format("fetchIndex = (uint)(%s(r[%u].%c));", rounded ? "round" : "trunc",
                  srcReg & 0x1F, kComponents[srcSwizzle]));
      Line(Format("fetchStride = %u;", stride));
    }
    Line(Format("fetchAddress = (uint)((int)(fetchIndex * fetchStride) + %d) * 4u;", offset));

    std::string bufferName = Format("vb%u", slot);
    std::string decoded;
    const std::string scale =
        exponent != 0
            ? Format(" * %.9g",
                     exponent > 0 ? static_cast<double>(1 << exponent)
                                  : 1.0 / static_cast<double>(1 << -exponent))
            : "";

    auto Normalised = [&](const std::string& value, int bits) -> std::string {
      if (isSigned) {
        std::string wide = Format("((int)((%s) << %d) >> %d)", value.c_str(), 32 - bits, 32 - bits);
        if (whole) return "(float)" + wide;
        return Format("max((float)%s / %.1f, -1.0)", wide.c_str(),
                      static_cast<double>((1u << (bits - 1)) - 1));
      }
      if (whole) return "(float)(" + value + ")";
      return Format("((float)(%s) / %.1f)", value.c_str(), static_cast<double>((1ULL << bits) - 1));
    };

    switch (format) {
      case 6:  // k_8_8_8_8
        Line(Format("fetchWord = bswap(%s.Load(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, %s, %s, %s)", Normalised("fetchWord & 0xFF", 8).c_str(),
                         Normalised("(fetchWord >> 8) & 0xFF", 8).c_str(),
                         Normalised("(fetchWord >> 16) & 0xFF", 8).c_str(),
                         Normalised("fetchWord >> 24", 8).c_str());
        break;
      case 7:  // k_2_10_10_10
        Line(Format("fetchWord = bswap(%s.Load(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, %s, %s, %s)", Normalised("fetchWord & 0x3FF", 10).c_str(),
                         Normalised("(fetchWord >> 10) & 0x3FF", 10).c_str(),
                         Normalised("(fetchWord >> 20) & 0x3FF", 10).c_str(),
                         Normalised("fetchWord >> 30", 2).c_str());
        break;
      case 16:  // k_10_11_11
        Line(Format("fetchWord = bswap(%s.Load(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, %s, %s, 1.0)", Normalised("fetchWord & 0x7FF", 11).c_str(),
                         Normalised("(fetchWord >> 11) & 0x7FF", 11).c_str(),
                         Normalised("fetchWord >> 22", 10).c_str());
        break;
      case 17:  // k_11_11_10
        Line(Format("fetchWord = bswap(%s.Load(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, %s, %s, 1.0)", Normalised("fetchWord & 0x3FF", 10).c_str(),
                         Normalised("(fetchWord >> 10) & 0x7FF", 11).c_str(),
                         Normalised("fetchWord >> 21", 11).c_str());
        break;
      case 25:  // k_16_16
        Line(Format("fetchWord = bswap(%s.Load(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, %s, 0.0, 1.0)", Normalised("fetchWord & 0xFFFF", 16).c_str(),
                         Normalised("fetchWord >> 16", 16).c_str());
        break;
      case 26:  // k_16_16_16_16
        Line(Format("fetchWords = bswap(%s.Load2(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, %s, %s, %s)", Normalised("fetchWords.x & 0xFFFF", 16).c_str(),
                         Normalised("fetchWords.x >> 16", 16).c_str(),
                         Normalised("fetchWords.y & 0xFFFF", 16).c_str(),
                         Normalised("fetchWords.y >> 16", 16).c_str());
        break;
      case 31:  // k_16_16_FLOAT
        Line(Format("fetchWord = bswap(%s.Load(fetchAddress));", bufferName.c_str()));
        decoded = "float4(f16tof32(fetchWord & 0xFFFF), f16tof32(fetchWord >> 16), 0.0, 1.0)";
        break;
      case 32:  // k_16_16_16_16_FLOAT
        Line(Format("fetchWords = bswap(%s.Load2(fetchAddress));", bufferName.c_str()));
        decoded =
            "float4(f16tof32(fetchWords.x & 0xFFFF), f16tof32(fetchWords.x >> 16), "
            "f16tof32(fetchWords.y & 0xFFFF), f16tof32(fetchWords.y >> 16))";
        break;
      case 33:  // k_32
        Line(Format("fetchWord = bswap(%s.Load(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, 0.0, 0.0, 1.0)", Normalised("fetchWord", 32).c_str());
        break;
      case 34:  // k_32_32
        Line(Format("fetchWords = bswap(%s.Load2(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, %s, 0.0, 1.0)", Normalised("fetchWords.x", 32).c_str(),
                         Normalised("fetchWords.y", 32).c_str());
        break;
      case 35:  // k_32_32_32_32
        Line(Format("fetchWords4 = bswap(%s.Load4(fetchAddress));", bufferName.c_str()));
        decoded = Format("float4(%s, %s, %s, %s)", Normalised("fetchWords4.x", 32).c_str(),
                         Normalised("fetchWords4.y", 32).c_str(),
                         Normalised("fetchWords4.z", 32).c_str(),
                         Normalised("fetchWords4.w", 32).c_str());
        break;
      case 36:  // k_32_FLOAT
        decoded = Format("float4(asfloat(bswap(%s.Load(fetchAddress))), 0.0, 0.0, 1.0)", bufferName.c_str());
        break;
      case 37:  // k_32_32_FLOAT
        decoded = Format("float4(asfloat(bswap(%s.Load2(fetchAddress))), 0.0, 1.0)", bufferName.c_str());
        break;
      case 57:  // k_32_32_32_FLOAT
        decoded = Format("float4(asfloat(bswap(%s.Load3(fetchAddress))), 1.0)", bufferName.c_str());
        break;
      case 38:  // k_32_32_32_32_FLOAT
        decoded = Format("asfloat(bswap(%s.Load4(fetchAddress)))", bufferName.c_str());
        break;
      default:
        out.problem = Format("vertex format %u", format);
        decoded = "float4(0.0, 0.0, 0.0, 1.0)";
        break;
    }

    Line(Format("fetched = %s%s;", decoded.c_str(), scale.c_str()));
    FetchWrite(dstReg, dstSwizzle, "fetched");

    if (isPredicated) {
      --indent;
      Line("}");
    }
    --indent;
    Line("}");
  }

  void TextureFetchInstruction(uint32_t d0, uint32_t d1, uint32_t d2) {
    const uint32_t opcode = d0 & 0x1F;
    const uint32_t srcReg = (d0 >> 5) & 0x3F;
    const uint32_t dstReg = (d0 >> 12) & 0x3F;
    const uint32_t slot = (d0 >> 20) & 0x1F;
    const bool unnormalised = ((d0 >> 25) & 1) != 0;
    const uint32_t srcSwizzle = (d0 >> 26) & 0x3F;
    const uint32_t dstSwizzle = d1 & 0xFFF;
    const bool useRegisterLod = ((d1 >> 29) & 1) != 0;
    const bool isPredicated = ((d1 >> 31) & 1) != 0;
    int lodBias = static_cast<int>((d2 >> 2) & 0x7F);
    if (lodBias >= 64) lodBias -= 128;
    auto offsetOf = [](uint32_t bits) {
      int value = static_cast<int>(bits & 0x1F);
      return value >= 16 ? value - 32 : value;
    };
    const int offsetX = offsetOf(d2 >> 16);
    const int offsetY = offsetOf(d2 >> 21);
    const uint32_t dimension = (d2 >> 14) & 3;
    const bool predicateCondition = ((d2 >> 31) & 1) != 0;

    if (opcode != 1) {
      const std::string coordinate =
          Format("r[%u].%c%c", srcReg & 0x1F, kComponents[srcSwizzle & 3],
                 kComponents[(srcSwizzle >> 2) & 3]);
      if (opcode == 24) {
        Line(Format("textureLod = r[%u].x;", srcReg & 0x1F));
      } else if (opcode == 17) {
        Line("fetched = float4(0.0, 0.0, 0.0, 0.0);");
      } else if (opcode == 18) {
        if (pixel) {
          Line(Format("fetched = float4(ddx(%s.x), ddy(%s.x), ddx(%s.y), ddy(%s.y));",
                      coordinate.c_str(), coordinate.c_str(), coordinate.c_str(),
                      coordinate.c_str()));
        } else {
          Line("fetched = float4(0.0, 0.0, 0.0, 0.0);");
        }
      } else if (opcode == 19) {
        Line(Format("fetched = float4(frac(%s * textureSize[%u].xy - 0.5), 0.0, 0.0);",
                    coordinate.c_str(), slot));
      } else if (opcode != 25 && opcode != 26) {
        out.problem = Format("fetch opcode %u", opcode);
        return;
      }
      if (opcode == 17 || opcode == 18 || opcode == 19) FetchWrite(dstReg, dstSwizzle, "fetched");
      return;
    }

    textureSlots.insert(slot);
    uint32_t samplerIndex = 0;
    bool known = false;
    for (const TextureFetch& other : out.textureFetches) {
      if (other.slot == slot) {
        known = true;
        samplerIndex = other.sampler;
        break;
      }
    }
    if (!known) {
      TextureFetch fetch;
      fetch.slot = slot;
      fetch.dimension = dimension;
      fetch.sampler = static_cast<uint32_t>(out.textureFetches.size());
      samplerIndex = fetch.sampler;
      out.textureFetches.push_back(fetch);
    }

    Line("{");
    ++indent;
    if (isPredicated) {
      Line(Format("if (p0 == %s) {", predicateCondition ? "true" : "false"));
      ++indent;
    }

    const std::string coordinate =
        Format("r[%u].%c%c%c", srcReg & 0x1F, kComponents[srcSwizzle & 3],
               kComponents[(srcSwizzle >> 2) & 3], kComponents[(srcSwizzle >> 4) & 3]);
    const std::string texture = Format("t%u", slot);
    const std::string sampler = Format("s%u", samplerIndex);
    const std::string size = Format("textureSize[%u]", slot);

    std::string location;
    if (dimension == 3) {
      location = Format("cubeDirection(%s)", coordinate.c_str());
    } else {
      location = unnormalised ? Format("((%s).xy * %s.zw)", coordinate.c_str(), size.c_str())
                              : Format("(%s).xy", coordinate.c_str());
    }
    if (dimension != 3 && (offsetX != 0 || offsetY != 0)) {
      location = Format("(%s + float2(%.4f, %.4f) * %s.zw)", location.c_str(), offsetX * 0.5,
                        offsetY * 0.5, size.c_str());
    }

    std::string sample;
    if (useRegisterLod || !pixel) {
      sample = Format("%s.SampleLevel(%s, %s, %s)", texture.c_str(), sampler.c_str(),
                      location.c_str(), useRegisterLod ? "textureLod" : "0.0");
    } else if (lodBias != 0) {
      sample = Format("%s.SampleBias(%s, %s, %.4f)", texture.c_str(), sampler.c_str(),
                      location.c_str(), lodBias / 16.0);
    } else {
      sample = Format("%s.Sample(%s, %s)", texture.c_str(), sampler.c_str(), location.c_str());
    }

    Line(Format("fetched = textureAdjust(%u, %s);", slot, sample.c_str()));
    FetchWrite(dstReg, dstSwizzle, "fetched");

    if (isPredicated) {
      --indent;
      Line("}");
    }
    --indent;
    Line("}");
  }

  void ExecBody(uint64_t instruction) {
    const uint32_t address = static_cast<uint32_t>(instruction) & 0xFFF;
    const uint32_t count = (static_cast<uint32_t>(instruction) >> 12) & 7;
    const uint32_t sequence = (static_cast<uint32_t>(instruction) >> 16) & 0xFFF;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t slot = address + i;
      if (slot * 3 + 2 >= words.size()) {
        out.problem = "instruction past the end";
        return;
      }
      const uint32_t d0 = words[slot * 3 + 0];
      const uint32_t d1 = words[slot * 3 + 1];
      const uint32_t d2 = words[slot * 3 + 2];
      const bool fetch = ((sequence >> (i * 2)) & 1) != 0;
      if (fetch) {
        if ((d0 & 0x1F) == 0) {
          VertexFetchInstruction(d0, d1, d2);
        } else {
          TextureFetchInstruction(d0, d1, d2);
        }
      } else {
        Alu(d0, d1, d2);
      }
    }
  }

  std::string BoolConstant(uint32_t index) {
    return Format("((bools[%u][%u] >> %u) & 1u)", index / 128, (index / 32) & 3, index & 31);
  }

  bool EmitStructured(const std::vector<uint64_t>& flow, uint32_t begin, uint32_t end,
                      bool inLoop) {
    auto jumpOf = [&](uint32_t i, uint32_t& target, bool& unconditional) -> bool {
      const uint64_t instruction = flow[i];
      if ((static_cast<uint32_t>(instruction >> 44) & 0xF) != 11) return false;
      target = static_cast<uint32_t>(instruction) & 0x1FFF;
      unconditional = ((instruction >> 13) & 1) != 0;
      return true;
    };
    const std::string finish = inLoop ? "break;" : "";
    uint32_t i = begin;
    while (i < end) {
      const uint64_t instruction = flow[i];
      const uint32_t opcode = static_cast<uint32_t>(instruction >> 44) & 0xF;
      switch (opcode) {
        case 0:
        case 12:
        case 15:
          ++i;
          break;
        case 1:
          ExecBody(instruction);
          ++i;
          break;
        case 2:
          ExecBody(instruction);
          if (inLoop) Line(finish);
          ++i;
          break;
        case 3:
        case 4: {
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFF;
          const bool condition = ((instruction >> 42) & 1) != 0;
          Line(Format("if (%s == %uu) {", BoolConstant(boolAddress).c_str(), condition ? 1u : 0u));
          ++indent;
          ExecBody(instruction);
          --indent;
          Line("}");
          if (opcode == 4 && inLoop) Line(finish);
          ++i;
          break;
        }
        case 5:
        case 6:
        case 13:
        case 14: {
          const bool condition = ((instruction >> 42) & 1) != 0;
          Line(Format("if (p0 == %s) {", condition ? "true" : "false"));
          ++indent;
          ExecBody(instruction);
          --indent;
          Line("}");
          if ((opcode == 6 || opcode == 14) && inLoop) Line(finish);
          ++i;
          break;
        }
        case 11: {
          uint32_t target = 0;
          bool unconditional = false;
          jumpOf(i, target, unconditional);
          if (target <= i || target > end) return false;
          if (unconditional) {
            i = target;
            break;
          }
          const bool onPredicate = ((instruction >> 14) & 1) != 0;
          const bool condition = ((instruction >> 42) & 1) != 0;
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFF;
          if (onPredicate) {
            Line(Format("if (p0 == %s) {", condition ? "false" : "true"));
          } else {
            Line(Format("if (%s == %uu) {", BoolConstant(boolAddress).c_str(), condition ? 0u : 1u));
          }
          ++indent;
          uint32_t elseTarget = 0;
          bool elseUnconditional = false;
          const uint32_t last = target - 1;
          const bool ifElse = last > i && jumpOf(last, elseTarget, elseUnconditional) &&
                              elseUnconditional && elseTarget > target && elseTarget <= end;
          if (!EmitStructured(flow, i + 1, ifElse ? last : target, inLoop)) return false;
          --indent;
          if (ifElse) {
            Line("} else {");
            ++indent;
            if (!EmitStructured(flow, target, elseTarget, inLoop)) return false;
            --indent;
          }
          Line("}");
          i = ifElse ? elseTarget : target;
          break;
        }
        case 7: {
          const uint32_t loopEnd = static_cast<uint32_t>(instruction) & 0x1FFF;
          const bool repeat = ((instruction >> 13) & 1) != 0;
          const uint32_t loopId = static_cast<uint32_t>(instruction >> 16) & 0x1F;
          if (loopEnd <= i || loopEnd >= end) return false;
          const uint64_t last = flow[loopEnd];
          if ((static_cast<uint32_t>(last >> 44) & 0xF) != 8 ||
              (static_cast<uint32_t>(last) & 0x1FFF) != i) {
            return false;
          }
          for (uint32_t j = i + 1; j < loopEnd; ++j) {
            const uint32_t inside = static_cast<uint32_t>(flow[j] >> 44) & 0xF;
            if (inside == 2 || inside == 4 || inside == 6 || inside == 14) return false;
          }
          const bool predicated = ((last >> 13) & 1) != 0;
          const bool condition = ((last >> 42) & 1) != 0;
          Line(Format("{ uint loopConstant%u = loops[%u][%u]; uint loopCount%u = loopConstant%u & 0xFFu;",
                      i, loopId / 4, loopId & 3, i, i));
          Line(Format("  int loopStep%u = (int)((loopConstant%u >> 16) & 0xFFu); if (loopStep%u >= 128) loopStep%u -= 256;",
                      i, i, i, i));
          Line(Format("  int savedAL%u = aL; aL = (int)((loopConstant%u >> 8) & 0xFFu);", i, i));
          Line(Format("  if (loopCount%u != 0u || %s) {", i, repeat ? "true" : "false"));
          Line(Format("  [loop] for (uint loopIndex%u = 0; loopIndex%u < 1024u; loopIndex%u++) {",
                      i, i, i));
          indent += 2;
          if (!EmitStructured(flow, i + 1, loopEnd, inLoop)) return false;
          Line(Format("aL += loopStep%u; if (loopIndex%u >= loopCount%u) break;", i, i, i));
          if (predicated) {
            Line(Format("if (p0 == %s) break;", condition ? "false" : "true"));
          }
          indent -= 2;
          Line("  } }");
          Line(Format("  aL = savedAL%u; }", i));
          i = loopEnd + 1;
          break;
        }
        default:
          return false;
      }
    }
    return true;
  }

  bool Run() {
    if (words.size() < 3) {
      out.problem = "no words";
      return false;
    }

    std::vector<uint64_t> flow;
    uint32_t firstTarget = static_cast<uint32_t>(words.size() / 3);
    for (uint32_t slot = 0; slot * 3 + 2 < words.size() && slot < firstTarget; ++slot) {
      const uint32_t a = words[slot * 3 + 0];
      const uint32_t b = words[slot * 3 + 1];
      const uint32_t c = words[slot * 3 + 2];
      const uint64_t first = static_cast<uint64_t>(a) | (static_cast<uint64_t>(b & 0xFFFFu) << 32);
      const uint64_t second =
          static_cast<uint64_t>(b >> 16) | (static_cast<uint64_t>(c) << 16);
      for (uint64_t f : {first, second}) {
        flow.push_back(f);
        const uint32_t op = static_cast<uint32_t>(f >> 44) & 0xFu;
        if ((op >= 1u && op <= 6u) || op == 13u || op == 14u) {
          const uint32_t addr = static_cast<uint32_t>(f) & 0xFFFu;
          if (addr < firstTarget) firstTarget = addr;
        }
      }
    }

    {
      const std::string savedBody = std::move(body);
      const int savedIndent = indent;
      const auto savedVertex = vertexSlots;
      const auto savedStrides = vertexStrides;
      const auto savedTextures = textureSlots;
      const auto savedExports = exportsUsed;
      const auto savedConstants = constantsRead;
      const bool savedRel = relativeConstants;
      const auto savedVtxFetches = out.vertexFetches;
      const auto savedTexFetches = out.textureFetches;
      const bool savedDepth = out.writesDepth;
      const bool savedKill = out.usesKill;
      const std::string savedProblem = out.problem;

      body.clear();
      lastFullVertexSlot = 0;
      Line("[loop] for (int structuredOnce = 0; structuredOnce < 1; structuredOnce++) {");
      ++indent;
      if (EmitStructured(flow, 0, static_cast<uint32_t>(flow.size()), true)) {
        --indent;
        Line("}");
        return out.problem.empty();
      }
      body = std::move(savedBody);
      indent = savedIndent;
      vertexSlots = savedVertex;
      vertexStrides = savedStrides;
      textureSlots = savedTextures;
      exportsUsed = savedExports;
      constantsRead = savedConstants;
      relativeConstants = savedRel;
      lastFullVertexSlot = 0;
      out.vertexFetches = savedVtxFetches;
      out.textureFetches = savedTexFetches;
      out.writesDepth = savedDepth;
      out.usesKill = savedKill;
      out.problem = savedProblem;
    }

    const uint32_t endCase = static_cast<uint32_t>(flow.size());
    Line("for (uint step = 0; step < 65536u; step++) {");
    ++indent;
    Line("switch (programCounter) {");
    for (size_t i = 0; i < flow.size(); ++i) {
      const uint64_t instruction = flow[i];
      const uint32_t opcode = static_cast<uint32_t>(instruction >> 44) & 0xF;
      const std::string next = Format("programCounter = %zu; break;", i + 1);
      const std::string finish = Format("programCounter = %u; break;", endCase);
      Line(Format("case %zu: {", i));
      ++indent;
      switch (opcode) {
        case 0:
        case 12:
        case 15:
          Line(next);
          break;
        case 1:
          ExecBody(instruction);
          Line(next);
          break;
        case 2:
          ExecBody(instruction);
          Line(finish);
          break;
        case 3:
        case 4: {
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFF;
          const bool condition = ((instruction >> 42) & 1) != 0;
          Line(Format("if (%s == %uu) {", BoolConstant(boolAddress).c_str(), condition ? 1u : 0u));
          ++indent;
          ExecBody(instruction);
          --indent;
          Line("}");
          Line(opcode == 4 ? finish : next);
          break;
        }
        case 5:
        case 6:
        case 13:
        case 14: {
          const bool condition = ((instruction >> 42) & 1) != 0;
          Line(Format("if (p0 == %s) {", condition ? "true" : "false"));
          ++indent;
          ExecBody(instruction);
          --indent;
          Line("}");
          Line((opcode == 6 || opcode == 14) ? finish : next);
          break;
        }
        case 7: {
          const uint32_t loopEnd = static_cast<uint32_t>(instruction) & 0x1FFF;
          const bool repeat = ((instruction >> 13) & 1) != 0;
          const uint32_t loopId = static_cast<uint32_t>(instruction >> 16) & 0x1F;
          Line(Format("uint loopConstant = loops[%u][%u];", loopId / 4, loopId & 3));
          Line("uint count = loopConstant & 0xFFu;");
          Line("if (count == 0u && !" + std::string(repeat ? "true" : "false") +
               ") { programCounter = " + std::to_string(loopEnd + 1) + "; break; }");
          Line("if (depth < 4u) { loopCount[depth] = count; loopAddress[depth] = aL; depth++; }");
          Line("aL = (int)((loopConstant >> 8) & 0xFFu);");
          Line(next);
          break;
        }
        case 8: {
          const uint32_t loopStart = static_cast<uint32_t>(instruction) & 0x1FFF;
          const uint32_t loopId = static_cast<uint32_t>(instruction >> 16) & 0x1F;
          const bool predicated = ((instruction >> 13) & 1) != 0;
          const bool condition = ((instruction >> 42) & 1) != 0;
          Line(Format("uint loopConstant = loops[%u][%u];", loopId / 4, loopId & 3));
          Line("int stepValue = (int)((loopConstant >> 16) & 0xFFu);");
          Line("if (stepValue >= 128) stepValue -= 256;");
          Line("bool exitLoop = false;");
          if (predicated) Line(Format("if (p0 == %s) exitLoop = true;", condition ? "false" : "true"));
          Line("if (!exitLoop && depth > 0u && loopCount[depth - 1] > 0u) { loopCount[depth - 1]--; aL += stepValue; programCounter = " +
               std::to_string(loopStart + 1) + "; break; }");
          Line("if (depth > 0u) { depth--; aL = loopAddress[depth]; }");
          Line(next);
          break;
        }
        case 11: {
          const uint32_t target = static_cast<uint32_t>(instruction) & 0x1FFF;
          const bool unconditional = ((instruction >> 13) & 1) != 0;
          const bool onPredicate = ((instruction >> 14) & 1) != 0;
          const bool condition = ((instruction >> 42) & 1) != 0;
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFF;
          if (unconditional) {
            Line(Format("programCounter = %u; break;", target));
          } else if (onPredicate) {
            Line(Format("if (p0 == %s) { programCounter = %u; break; }",
                        condition ? "true" : "false", target));
          } else {
            Line(Format("if (%s == %uu) { programCounter = %u; break; }",
                        BoolConstant(boolAddress).c_str(), condition ? 1u : 0u, target));
          }
          Line(next);
          break;
        }
        case 9:
        case 10:
          out.problem = "subroutine call";
          return false;
        default:
          out.problem = Format("control flow opcode %u", opcode);
          return false;
      }
      --indent;
      Line("}");
    }
    Line(Format("default: programCounter = %u; break;", endCase));
    Line("}");
    Line(Format("if (programCounter >= %u) break;", endCase));
    --indent;
    Line("}");
    return true;
  }
};

}  // namespace

namespace bs1::renderer::XenosHlsl {

bool Depth24() { return false; }

const char* const Depth20e4Source =
    "float Depth20e4(float z) {\n"
    "    uint f = asuint(z);\n"
    "    if (f < 0x38800000u) return floor(z * 17179869184.0) * (1.0 / 17179869184.0);\n"
    "    return asfloat(f & 0xFFFFFFF8u);\n"
    "}\n";

uint64_t Version() {
  static const char* const tag = "bs1-xenos-hlsl-v9-precise-vf";
  uint64_t hash = 14695981039346656037ULL;
  for (const char* p = tag; *p; ++p) {
    hash ^= static_cast<unsigned char>(*p);
    hash *= 1099511628211ULL;
  }
  return hash;
}

Translation Translate(const std::vector<uint32_t>& words, bool pixel) {
  Translator translator(words, pixel);
  const bool ran = translator.Run();
  Translation& out = translator.out;
  if (!ran || !out.problem.empty()) {
    out.ok = false;
    return out;
  }
  translator.PackConstants();

  std::string hlsl;
  hlsl += "// Translated from Xenos microcode by BioShock1 custom D3D11 renderer.\n";
  hlsl += "cbuffer FloatConstants : register(b0) { float4 c[256]; };\n";
  hlsl += "cbuffer BoolConstants : register(b1) { uint4 bools[2]; uint4 loops[8]; };\n";
  hlsl +=
      "cbuffer DrawConstants : register(b2) {\n"
      "    float4 viewportScale;\n"
      "    float4 viewportOffset;\n"
      "    float4 targetSize;\n"
      "    uint4 flags;\n"
      "    float4 textureSize[32];\n"
      "    uint4 textureAdjustment[32];\n"
      "    float4 pixelGen;\n"
      "    float4 colorExpBias;\n"
      "};\n";
  hlsl +=
      "uint bswap(uint v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }\n"
      "uint2 bswap(uint2 v) { return uint2(bswap(v.x), bswap(v.y)); }\n"
      "uint3 bswap(uint3 v) { return uint3(bswap(v.x), bswap(v.y), bswap(v.z)); }\n"
      "uint4 bswap(uint4 v) { return uint4(bswap(v.x), bswap(v.y), bswap(v.z), bswap(v.w)); }\n";
  hlsl +=
      "float4 select4(bool4 v) { return float4(v.x ? 1.0 : 0.0, v.y ? 1.0 : 0.0, v.z ? 1.0 : 0.0, v.w ? 1.0 : 0.0); }\n"
      "float max4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }\n"
      "float4 blend4(float4 mask, float4 whenSet, float4 whenClear) { return whenSet * mask + whenClear * (1.0 - mask); }\n"
      "float blend1(bool set, float whenSet, float whenClear) { float mask = set ? 1.0 : 0.0; return whenSet * mask + whenClear * (1.0 - mask); }\n"
      "float4 mulL(float4 a, float4 b) { float4 r = a * b; return (a == 0.0 || b == 0.0) ? float4(0.0, 0.0, 0.0, 0.0) : r; }\n"
      "float mulL1(float a, float b) { float r = a * b; return (a == 0.0 || b == 0.0) ? 0.0 : r; }\n";
  hlsl +=
      "float flipIf(float v, bool flip) { return asfloat(asuint(v) ^ (flip ? 0x80000000u : 0u)); }\n"
      "float4 cubeMapX(float3 v) { return float4(flipIf(v.z, v.x >= 0.0), flipIf(v.y, true), v.x >= 0.0 ? 0.0 : 1.0, 2.0 * abs(v.x)); }\n"
      "float4 cubeMapY(float3 v) { return float4(v.x, flipIf(v.z, v.y < 0.0), v.y >= 0.0 ? 2.0 : 3.0, 2.0 * abs(v.y)); }\n"
      "float4 cubeMapZ(float3 v) { return float4(flipIf(v.x, v.z < 0.0), flipIf(v.y, true), v.z >= 0.0 ? 4.0 : 5.0, 2.0 * abs(v.z)); }\n"
      "float4 cubeMap(float4 a, float4 b) {\n"
      "    return (abs(a.x) >= abs(a.y) && abs(a.x) >= abs(a.z)) ? cubeMapX(a.xyz) : (abs(a.y) >= abs(a.z)) ? cubeMapY(a.xyz) : cubeMapZ(a.xyz); }\n";
  hlsl +=
      "float3 cubeDirection(float3 c) {\n"
      "    uint face = (uint)c.z;\n"
      "    float3 major = face == 0 ? float3(1.0, 0.0, 0.0) : face == 1 ? float3(-1.0, 0.0, 0.0) : face == 2 ? float3(0.0, 1.0, 0.0)\n"
      "        : face == 3 ? float3(0.0, -1.0, 0.0) : face == 4 ? float3(0.0, 0.0, 1.0) : float3(0.0, 0.0, -1.0);\n"
      "    float3 sx = face == 0 ? float3(0.0, 0.0, -1.0) : face == 1 ? float3(0.0, 0.0, 1.0) : face == 5 ? float3(-1.0, 0.0, 0.0) : float3(1.0, 0.0, 0.0);\n"
      "    float3 sy = face == 2 ? float3(0.0, 0.0, 1.0) : face == 3 ? float3(0.0, 0.0, -1.0) : float3(0.0, -1.0, 0.0);\n"
      "    return major + sx * c.x + sy * c.y; }\n";

  for (const TextureFetch& fetch : out.textureFetches) {
    const char* type = fetch.dimension == 3 ? "TextureCube" : "Texture2D";
    hlsl += Format("%s t%u : register(t%u);\nSamplerState s%u : register(s%u);\n", type,
                   fetch.slot, fetch.slot, fetch.sampler, fetch.sampler);
  }

  hlsl +=
      "float pickComponent(float4 v, uint s) { return s == 0 ? v.x : s == 1 ? v.y : s == 2 ? v.z : s == 3 ? v.w : s == 4 ? 0.0 : 1.0; }\n"
      "float adjustComponent(float4 v, uint s, uint mode) {\n"
      "    return mode == 2 ? pickComponent(v, s) * 2.0 - 1.0 : mode == 3 ? pow(abs(pickComponent(v, s)), 2.2) : pickComponent(v, s);\n"
      "}\n"
      "float4 textureAdjust(uint slot, float4 v) {\n"
      "    return float4(adjustComponent(v, textureAdjustment[slot].x & 7u, textureAdjustment[slot].y & 3u),\n"
      "        adjustComponent(v, (textureAdjustment[slot].x >> 3) & 7u, (textureAdjustment[slot].y >> 2) & 3u),\n"
      "        adjustComponent(v, (textureAdjustment[slot].x >> 6) & 7u, (textureAdjustment[slot].y >> 4) & 3u),\n"
      "        adjustComponent(v, (textureAdjustment[slot].x >> 9) & 7u, (textureAdjustment[slot].y >> 6) & 3u)) *\n"
      "        asfloat(textureAdjustment[slot].z ^ 0x3F800000u);\n"
      "}\n";

  const bool depth24 = Depth24();
  if (pixel) {
    const bool cutDepth = depth24 && !out.writesDepth;
    if (cutDepth) hlsl += Depth20e4Source;
    hlsl += cutDepth ? "struct Input { noperspective centroid float4 position : SV_Position;"
                     : "struct Input { float4 position : SV_Position;";
    for (uint32_t i = 0; i < 16; ++i) hlsl += Format(" float4 o%u : TEXCOORD%u;", i, i);
    hlsl += " bool front : SV_IsFrontFace; };\n";
    hlsl +=
        "struct Output { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;";
    if (out.writesDepth) {
      hlsl += " float depth : SV_Depth;";
    } else if (cutDepth) {
      hlsl += "\n#ifndef XENOS_NO_DEPTH\n float depth : SV_DepthLessEqual;\n#endif\n";
    }
    hlsl += " };\n";
    hlsl += "Output main(Input input) {\n";
    hlsl += "    float4 r[32];\n";
    for (uint32_t i = 0; i < 32; ++i) {
      hlsl += i < 16 ? Format("    r[%u] = input.o%u;\n", i, i)
                     : Format("    r[%u] = float4(0.0, 0.0, 0.0, 0.0);\n", i);
    }
    hlsl +=
        "    if (pixelGen.w != 0.0) {\n"
        "        float2 at = floor(input.position.xy) * pixelGen.xy;\n"
        "        float4 generated = float4(asfloat(asuint(at.x) | (input.front ? 0u : 0x80000000u)), at.y, 0.0, 0.0);\n"
        "        uint slot = uint(pixelGen.z);\n"
        "        [unroll] for (uint i = 0; i < 32; i++) if (i == slot) r[i] = generated;\n"
        "    }\n";
    hlsl +=
        "    float4 oC0 = float4(0.0, 0.0, 0.0, 0.0), oC1 = oC0, oC2 = oC0, oC3 = oC0, oDepth4 = oC0, oUnused = oC0;\n";
  } else {
    for (uint32_t slot : translator.vertexSlots) {
      hlsl += Format("ByteAddressBuffer vb%u : register(t%u);\n", slot, 32 + slot);
      VertexFetch fetch;
      fetch.slot = slot;
      if (translator.vertexStrides.count(slot)) fetch.stride = translator.vertexStrides[slot];
      out.vertexFetches.push_back(fetch);
    }
    hlsl += "struct Output { float4 position : SV_Position;";
    for (uint32_t i = 0; i < 16; ++i) hlsl += Format(" float4 o%u : TEXCOORD%u;", i, i);
    hlsl += " };\n";
    hlsl += "Output main(uint vertexId : SV_VertexID) {\n";
    hlsl += "    float4 r[32];\n";
    hlsl += "    r[0] = float4((float)vertexId, 0.0, 0.0, 0.0);\n";
    for (uint32_t i = 1; i < 32; ++i) hlsl += Format("    r[%u] = float4(0.0, 0.0, 0.0, 0.0);\n", i);
    hlsl += "    precise float4 oPos = float4(0.0, 0.0, 0.0, 1.0);\n";
    hlsl += "    float4 oPointSize = float4(0.0, 0.0, 0.0, 1.0), oUnused = float4(0.0, 0.0, 0.0, 1.0);\n";
    for (uint32_t i = 0; i < 16; ++i) hlsl += Format("    float4 o%u = float4(0.0, 0.0, 0.0, 0.0);\n", i);
    hlsl +=
        "    uint fetchIndex = 0, fetchStride = 0, fetchAddress = 0, fetchWord = 0; "
        "uint2 fetchWords = uint2(0, 0); uint4 fetchWords4 = uint4(0, 0, 0, 0);\n";
  }

  hlsl +=
      "    float4 fetched = float4(0.0, 0.0, 0.0, 0.0); float ps = 0.0; float textureLod = 0.0; "
      "int a0 = 0; bool p0 = false;\n";
  hlsl +=
      "    int programCounter = 0; int aL = 0; uint depth = 0; uint loopCount[4]; int loopAddress[4]; "
      "loopCount[0] = 0; loopCount[1] = 0; loopCount[2] = 0; loopCount[3] = 0; "
      "loopAddress[0] = 0; loopAddress[1] = 0; loopAddress[2] = 0; loopAddress[3] = 0;\n";
  hlsl += translator.body;

  if (pixel) {
    out.colourTargets = 0;
    for (uint32_t e : translator.exportsUsed) {
      if (e < 4) out.colourTargets |= 1u << e;
    }
    if (out.colourTargets == 0) out.colourTargets = 1;
    hlsl += "    Output output;\n";
    hlsl +=
        "    { uint fn = flags.y; float ref = asfloat(flags.z); float a = oC0.w; bool passes = true;\n"
        "      if (fn == 0) passes = false; else if (fn == 1) passes = a < ref; else if (fn == 2) passes = a == ref;\n"
        "      else if (fn == 3) passes = a <= ref; else if (fn == 4) passes = a > ref; else if (fn == 5) passes = a != ref;\n"
        "      else if (fn == 6) passes = a >= ref;\n"
        "      if (!passes) discard; }\n";
    hlsl +=
        "    output.c0 = oC0 * colorExpBias.x; output.c1 = oC1 * colorExpBias.y;\n"
        "    output.c2 = oC2 * colorExpBias.z; output.c3 = oC3 * colorExpBias.w;\n";
    if (out.writesDepth) {
      hlsl += "    output.depth = oDepth4.x;\n";
    } else if (depth24) {
      hlsl += "#ifndef XENOS_NO_DEPTH\n    output.depth = Depth20e4(input.position.z);\n#endif\n";
    }
    hlsl += "    return output;\n}\n";
  } else {
    hlsl +=
        "    Output output;\n"
        "    { uint vte = flags.x; precise float4 p = oPos;\n"
        "      precise float ww = (vte & 0x400u) ? p.w : (abs(p.w) > 1e-30 ? 1.0 / p.w : 1.0);\n"
        "      if (vte & 0x100u) { p.x *= ww; p.y *= ww; }\n"
        "      if (vte & 0x200u) { p.z *= ww; }\n"
        "      precise float xs = (vte & 0x1u) ? viewportScale.x : 1.0; precise float xo = (vte & 0x2u) ? viewportOffset.x : 0.0;\n"
        "      precise float ys = (vte & 0x4u) ? viewportScale.y : 1.0; precise float yo = (vte & 0x8u) ? viewportOffset.y : 0.0;\n"
        "      precise float zs = (vte & 0x10u) ? viewportScale.z : 1.0; precise float zo = (vte & 0x20u) ? viewportOffset.z : 0.0;\n"
        "      precise float centre = (vte & 0x10000u) ? 0.0 : 0.5;\n"
        "      precise float W = targetSize.x, H = targetSize.y;\n"
        "      precise float cx = p.x * (2.0 * xs / W) + ww * (2.0 * (xo + centre) / W - 1.0);\n"
        "      precise float cy = -(p.y * (2.0 * ys / H) + ww * (2.0 * (yo + centre) / H - 1.0));\n"
        "      precise float cz = p.z * zs + zo * ww;\n"
        "      output.position = float4(cx, cy, cz, ww); }\n";
    for (uint32_t i = 0; i < 16; ++i) hlsl += Format("    output.o%u = o%u;\n", i, i);
    hlsl += "    return output;\n}\n";
    uint32_t interpolators = 0;
    for (uint32_t e : translator.exportsUsed) {
      if (e < 16) interpolators = std::max(interpolators, e + 1);
    }
    out.interpolators = interpolators;
  }

  out.hlsl = hlsl;
  out.ok = true;
  return out;
}

}  // namespace bs1::renderer::XenosHlsl
