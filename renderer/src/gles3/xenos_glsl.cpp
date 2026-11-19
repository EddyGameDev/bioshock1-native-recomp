#include "xenos_glsl.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace {

using namespace bs1::renderer::XenosGlsl;

const char* const kComponents = "xyzw";

std::string Format(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  return buffer;
}

std::string BroadcastScalar(const std::string& expr, size_t lanes) {
  if (lanes == 1) return expr;
  return Format("vec%zu(%s)", lanes, expr.c_str());
}

struct Translator {
  const std::vector<uint32_t>& words;
  const bool pixel;
  Translation out;
  std::string body;
  std::vector<uint32_t> vertexSlotOrder;
  std::map<uint32_t, uint32_t> vertexSlotToBinding;
  std::map<uint32_t, uint32_t> vertexStrides;
  std::set<uint32_t> exportsUsed;
  int indent = 1;

  std::set<uint32_t> constantsRead;
  bool relativeConstants = false;
  uint32_t lastFullVertexSlot = 0;

  Translator(const std::vector<uint32_t>& w, bool p) : words(w), pixel(p) {}

  uint32_t RegisterVertexSlot(uint32_t slot) {
    auto it = vertexSlotToBinding.find(slot);
    if (it != vertexSlotToBinding.end()) return it->second;
    const uint32_t binding = static_cast<uint32_t>(vertexSlotOrder.size());
    vertexSlotOrder.push_back(slot);
    vertexSlotToBinding[slot] = binding;
    return binding;
  }

  std::string Constant(uint32_t index, bool relative) {
    const char* arr = pixel ? "pc" : "vc";
    if (relative) {
      relativeConstants = true;
      return Format("%s[(a0 + %d) & 255]", arr, static_cast<int>(index & 0xFFu));
    }
    constantsRead.insert(index & 0xFFu);
    return Format("%s[%u]", arr, index & 0xFFu);
  }

  void PackConstants() {
    if (relativeConstants || constantsRead.empty()) return;
    const std::string prefix = pixel ? "pc[" : "vc[";
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
      if (!word && body.compare(i, prefix.size(), prefix) == 0 &&
          i + prefix.size() < body.size() &&
          std::isdigit(static_cast<unsigned char>(body[i + prefix.size()]))) {
        size_t end = i + prefix.size();
        while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end]))) ++end;
        if (end < body.size() && body[end] == ']') {
          const uint32_t index =
              static_cast<uint32_t>(std::strtoul(body.c_str() + i + prefix.size(), nullptr, 10)) & 0xFFu;
          packed += Format("%s%u]", prefix.c_str(), position[index]);
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
      base = (reg & 0x40u) ? Format("r[(%d + aL) & 31]", static_cast<int>(reg & 0x1Fu))
                           : Format("r[%u]", reg & 0x1Fu);
    } else {
      base = Constant(reg, relative);
    }
    std::string swizzled = base + ".";
    for (uint32_t i = 0; i < 4; ++i) {
      swizzled += kComponents[(i + ((swizzle >> (i * 2)) & 3u)) & 3u];
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
      base = (reg & 0x40u) ? Format("r[(%d + aL) & 31]", static_cast<int>(reg & 0x1Fu))
                           : Format("r[%u]", reg & 0x1Fu);
    } else {
      base = Constant(reg, relative);
    }
    const uint32_t laneA = vectorTakesThree ? 2u : 3u;
    const uint32_t laneB = vectorTakesThree ? 3u : 0u;
    a = base + "." + kComponents[(laneA + ((swizzle >> (laneA * 2)) & 3u)) & 3u];
    b = base + "." + kComponents[(laneB + ((swizzle >> (laneB * 2)) & 3u)) & 3u];
    single = base + "." + kComponents[(3u + ((swizzle >> 6) & 3u)) & 3u];
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
    if (!exported) return Format("r[%u]", reg & 0x1Fu);
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
    const uint32_t vectorDest = d0 & 0x3Fu;
    const bool absoluteConstants = ((d0 >> 7) & 1u) != 0;
    const uint32_t scalarDest = (d0 >> 8) & 0x3Fu;
    const bool exported = ((d0 >> 15) & 1u) != 0;
    const uint32_t vectorMask = (d0 >> 16) & 0xFu;
    const uint32_t scalarMask = (d0 >> 20) & 0xFu;
    const bool vectorClamp = ((d0 >> 24) & 1u) != 0;
    const bool scalarClamp = ((d0 >> 25) & 1u) != 0;
    const uint32_t scalarOpcode = (d0 >> 26) & 0x3Fu;

    const uint32_t swizzle3 = d1 & 0xFFu;
    const uint32_t swizzle2 = (d1 >> 8) & 0xFFu;
    const uint32_t swizzle1 = (d1 >> 16) & 0xFFu;
    const bool negate3 = ((d1 >> 24) & 1u) != 0;
    const bool negate2 = ((d1 >> 25) & 1u) != 0;
    const bool negate1 = ((d1 >> 26) & 1u) != 0;
    const bool predicateCondition = ((d1 >> 27) & 1u) != 0;
    const bool isPredicated = ((d1 >> 28) & 1u) != 0;
    const bool relative0 = ((d1 >> 31) & 1u) != 0;
    const bool relative1 = ((d1 >> 30) & 1u) != 0;

    const uint32_t reg3 = d2 & 0xFFu;
    const uint32_t reg2 = (d2 >> 8) & 0xFFu;
    const uint32_t reg1 = (d2 >> 16) & 0xFFu;
    const uint32_t vectorOpcode = (d2 >> 24) & 0x1Fu;
    const bool temp3 = ((d2 >> 29) & 1u) != 0;
    const bool temp2 = ((d2 >> 30) & 1u) != 0;
    const bool temp1 = ((d2 >> 31) & 1u) != 0;

    const bool rel1 = !temp1 && relative0;
    const bool rel2 = !temp2 && (temp1 ? relative0 : relative1);
    const bool rel3 = !temp3 && ((temp1 && temp2) ? relative0 : relative1);

    const std::string a = Source(reg1, temp1, swizzle1, negate1,
                                 temp1 ? (reg1 & 0x80u) != 0 : absoluteConstants, rel1);
    const std::string b = Source(reg2, temp2, swizzle2, negate2,
                                 temp2 ? (reg2 & 0x80u) != 0 : absoluteConstants, rel2);
    const std::string c = Source(reg3, temp3, swizzle3, negate3,
                                 temp3 ? (reg3 & 0x80u) != 0 : absoluteConstants, rel3);
    std::string sa, sb, s1;
    const bool vectorTakesThree =
        vectorOpcode == 11 || (vectorOpcode >= 12 && vectorOpcode <= 14) || vectorOpcode == 17;
    ScalarPair(reg3, temp3, swizzle3, negate3, temp3 ? (reg3 & 0x80u) != 0 : absoluteConstants,
               rel3, vectorTakesThree, sa, sb, s1);

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
      case 4:  vector = Format("vec4(equal(%s, %s))", a.c_str(), b.c_str()); break;
      case 5:  vector = Format("vec4(greaterThan(%s, %s))", a.c_str(), b.c_str()); break;
      case 6:  vector = Format("vec4(greaterThanEqual(%s, %s))", a.c_str(), b.c_str()); break;
      case 7:  vector = Format("vec4(notEqual(%s, %s))", a.c_str(), b.c_str()); break;
      case 8:  vector = Format("fract(%s)", a.c_str()); break;
      case 9:  vector = Format("trunc(%s)", a.c_str()); break;
      case 10: vector = Format("floor(%s)", a.c_str()); break;
      case 11: vector = Format("(mulL(%s, %s) + %s)", a.c_str(), b.c_str(), c.c_str()); break;
      case 12: vector = Format("blend4(vec4(equal(%s, vec4(0.0))), %s, %s)", a.c_str(), b.c_str(), c.c_str()); break;
      case 13: vector = Format("blend4(vec4(greaterThanEqual(%s, vec4(0.0))), %s, %s)", a.c_str(), b.c_str(), c.c_str()); break;
      case 14: vector = Format("blend4(vec4(greaterThan(%s, vec4(0.0))), %s, %s)", a.c_str(), b.c_str(), c.c_str()); break;
      case 15: vector = Format("vec4(dot(mulL(%s, %s), vec4(1.0)))", a.c_str(), b.c_str()); break;
      case 16: vector = Format("vec4(dot(mulL(%s, %s).xyz, vec3(1.0)))", a.c_str(), b.c_str()); break;
      case 17: vector = Format("vec4(dot(mulL(%s, %s).xy, vec2(1.0)) + (%s).x)", a.c_str(), b.c_str(), c.c_str()); break;
      case 18: vector = Format("cubeMap(%s, %s)", a.c_str(), b.c_str()); break;
      case 19: vector = Format("vec4(max4(%s))", a.c_str()); break;
      case 20:
        vector = Format("vec4((%s).w == 0.0 ? 0.0 : (%s).x + 1.0)", a.c_str(), b.c_str());
        Line(Format("p0 = ((%s).w == 0.0) && ((%s).x == 0.0);", a.c_str(), b.c_str()));
        break;
      case 21:
        vector = Format("vec4((%s).w == 0.0 ? 0.0 : (%s).x + 1.0)", a.c_str(), b.c_str());
        Line(Format("p0 = ((%s).w == 0.0) && ((%s).x != 0.0);", a.c_str(), b.c_str()));
        break;
      case 22:
        vector = Format("vec4((%s).w == 0.0 ? 0.0 : (%s).x + 1.0)", a.c_str(), b.c_str());
        Line(Format("p0 = ((%s).w == 0.0) && ((%s).x > 0.0);", a.c_str(), b.c_str()));
        break;
      case 23:
        vector = Format("vec4((%s).w == 0.0 ? 0.0 : (%s).x + 1.0)", a.c_str(), b.c_str());
        Line(Format("p0 = ((%s).w == 0.0) && ((%s).x >= 0.0);", a.c_str(), b.c_str()));
        break;
      case 24: vector = Format("vec4(equal(%s, %s))", a.c_str(), b.c_str()); kill = true; break;
      case 25: vector = Format("vec4(greaterThan(%s, %s))", a.c_str(), b.c_str()); kill = true; break;
      case 26: vector = Format("vec4(greaterThanEqual(%s, %s))", a.c_str(), b.c_str()); kill = true; break;
      case 27: vector = Format("vec4(notEqual(%s, %s))", a.c_str(), b.c_str()); kill = true; break;
      case 28: vector = Format("vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a.c_str(), b.c_str(), a.c_str(), b.c_str()); break;
      case 29:
        vector = Format("max(%s, %s)", a.c_str(), b.c_str());
        Line(Format("a0 = clamp(int(floor((%s).w + 0.5)), -256, 255);", a.c_str()));
        break;
      default:
        out.problem = Format("vector opcode %u", vectorOpcode);
        vector = "vec4(0.0)";
        break;
    }

    if (kill && pixel) {
      out.usesKill = true;
      Line(Format("if (any(notEqual(%s, vec4(0.0)))) discard;", vector.c_str()));
    }

    std::string vectorDestination;
    if (vectorMask != 0) {
      if (vectorClamp) vector = "clamp(" + vector + ", vec4(0.0), vec4(1.0))";
      vectorDestination = Destination(vectorDest, exported);
      Line(Format("vec4 vectorValue = (%s);", vector.c_str()));
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
      case 11: scalar = Format("fract(%s)", s1.c_str()); break;
      case 12: scalar = Format("trunc(%s)", s1.c_str()); break;
      case 13: scalar = Format("floor(%s)", s1.c_str()); break;
      case 14: scalar = Format("exp2(%s)", s1.c_str()); break;
      case 15:
      case 16: scalar = Format("(%s > 0.0 ? log2(%s) : -3.402823466e38)", s1.c_str(), s1.c_str()); break;
      case 17: scalar = Format("(%s == 0.0 ? 3.402823466e38 : 1.0 / %s)", s1.c_str(), s1.c_str()); break;
      case 18: scalar = Format("(%s == 0.0 ? 0.0 : 1.0 / %s)", s1.c_str(), s1.c_str()); break;
      case 19: scalar = Format("(1.0 / %s)", s1.c_str()); break;
      case 20: scalar = Format("(%s <= 0.0 ? 3.402823466e38 : inversesqrt(%s))", s1.c_str(), s1.c_str()); break;
      case 21: scalar = Format("(%s <= 0.0 ? 0.0 : inversesqrt(%s))", s1.c_str(), s1.c_str()); break;
      case 22: scalar = Format("inversesqrt(%s)", s1.c_str()); break;
      case 23:
        scalar = Format("max(%s, %s)", sa.c_str(), sb.c_str());
        Line(Format("a0 = clamp(int(floor(%s + 0.5)), -256, 255);", sa.c_str()));
        break;
      case 24:
        scalar = Format("max(%s, %s)", sa.c_str(), sb.c_str());
        Line(Format("a0 = clamp(int(floor(%s)), -256, 255);", sa.c_str()));
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
            (scalarOpcode & 1u) | (static_cast<uint32_t>(temp3) << 1) | (swizzle3 & 0x3Cu);
        std::string constant = Constant(reg3, rel3);
        constant += ".";
        constant += kComponents[(3u + ((swizzle3 >> 6) & 3u)) & 3u];
        std::string other = Format("r[%u].", temporary & 0x1Fu);
        other += kComponents[swizzle3 & 3u];
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
    if (scalarClamp) scalar = "clamp(" + scalar + ", 0.0, 1.0)";
    Line(Format("ps = %s;", scalar.c_str()));

    if (vectorMask != 0) {
      const std::string m = Mask(vectorMask);
      Line(Format("%s.%s = vectorValue.%s;", vectorDestination.c_str(), m.c_str(), m.c_str()));
    }
    if (scalarMask != 0) {
      const std::string destination = Destination(exported ? vectorDest : scalarDest, exported);
      const std::string m = Mask(scalarMask);
      Line(Format("%s.%s = %s;", destination.c_str(), m.c_str(),
                  BroadcastScalar("ps", m.size()).c_str()));
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
      const uint32_t selector = (swizzle >> (i * 3)) & 7u;
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
      Line(Format("r[%u].%c = %s;", destReg & 0x1Fu, kComponents[i], value.c_str()));
    }
  }

  void VertexFetchInstruction(uint32_t d0, uint32_t d1, uint32_t d2) {
    const uint32_t srcReg = (d0 >> 5) & 0x3Fu;
    const uint32_t dstReg = (d0 >> 12) & 0x3Fu;
    const uint32_t rawSlot = ((d0 >> 20) & 0x1Fu) * 3u + ((d0 >> 25) & 3u);
    const uint32_t srcSwizzle = (d0 >> 30) & 3u;
    const uint32_t dstSwizzle = d1 & 0xFFFu;
    const bool isSigned = ((d1 >> 12) & 1u) != 0;
    const bool whole = ((d1 >> 13) & 1u) != 0;
    const bool rounded = ((d1 >> 15) & 1u) != 0;
    const uint32_t format = (d1 >> 16) & 0x3Fu;
    int exponent = static_cast<int>((d1 >> 24) & 0x3Fu);
    if (exponent >= 32) exponent -= 64;
    const bool mini = ((d1 >> 30) & 1u) != 0;
    const bool isPredicated = ((d1 >> 31) & 1u) != 0;
    const uint32_t stride = d2 & 0xFFu;
    int32_t offset = static_cast<int32_t>((d2 >> 8) & 0x7FFFFFu);
    if (offset >= 0x400000) offset -= 0x800000;
    const bool predicateCondition = ((d2 >> 31) & 1u) != 0;

    const uint32_t slot = mini ? lastFullVertexSlot : rawSlot;
    if (!mini) lastFullVertexSlot = rawSlot;

    const uint32_t binding = RegisterVertexSlot(slot);
    if (!mini && stride != 0 && vertexStrides.count(slot) == 0) vertexStrides[slot] = stride;

    Line("{");
    ++indent;
    if (isPredicated) {
      Line(Format("if (p0 == %s) {", predicateCondition ? "true" : "false"));
      ++indent;
    }

    if (!mini) {
      Line(Format("fetchIndex = uint(%s(r[%u].%c));", rounded ? "round" : "trunc",
                  srcReg & 0x1Fu, kComponents[srcSwizzle]));
      Line(Format("fetchStride = %uu;", stride));
    }
    Line(Format("fetchWordIdx = uint(int(fetchIndex * fetchStride) + %d);", offset));

    const std::string buf = Format("vb%u.d", binding);
    const auto load1 = [&]() {
      return Format("bswap(%s[min(fetchWordIdx, uint(%s.length()) - 1u)])",
                    buf.c_str(), buf.c_str());
    };
    const auto loadAt = [&](uint32_t delta) {
      return Format("bswap(%s[min(fetchWordIdx + %uu, uint(%s.length()) - 1u)])",
                    buf.c_str(), delta, buf.c_str());
    };

    std::string decoded;
    const std::string scale =
        exponent != 0
            ? Format(" * %.9g",
                     exponent > 0 ? static_cast<double>(1 << exponent)
                                  : 1.0 / static_cast<double>(1 << -exponent))
            : "";

    auto Normalised = [&](const std::string& value, int bits) -> std::string {
      if (isSigned) {
        std::string wide =
            bits == 32 ? Format("int(%s)", value.c_str())
                       : Format("(int((%s) << %du) >> %d)", value.c_str(), 32 - bits, 32 - bits);
        if (whole) return "float(" + wide + ")";
        return Format("max(float(%s) / %.1f, -1.0)", wide.c_str(),
                      static_cast<double>((1ULL << (bits - 1)) - 1));
      }
      if (whole) return "float(" + value + ")";
      return Format("(float(%s) / %.1f)", value.c_str(),
                    static_cast<double>((1ULL << bits) - 1));
    };

    switch (format) {
      case 6:  // k_8_8_8_8
        Line(Format("fetchWord = %s;", load1().c_str()));
        decoded = Format("vec4(%s, %s, %s, %s)", Normalised("fetchWord & 0xFFu", 8).c_str(),
                         Normalised("(fetchWord >> 8u) & 0xFFu", 8).c_str(),
                         Normalised("(fetchWord >> 16u) & 0xFFu", 8).c_str(),
                         Normalised("fetchWord >> 24u", 8).c_str());
        break;
      case 7:  // k_2_10_10_10
        Line(Format("fetchWord = %s;", load1().c_str()));
        decoded = Format("vec4(%s, %s, %s, %s)", Normalised("fetchWord & 0x3FFu", 10).c_str(),
                         Normalised("(fetchWord >> 10u) & 0x3FFu", 10).c_str(),
                         Normalised("(fetchWord >> 20u) & 0x3FFu", 10).c_str(),
                         Normalised("fetchWord >> 30u", 2).c_str());
        break;
      case 16:  // k_10_11_11
        Line(Format("fetchWord = %s;", load1().c_str()));
        decoded = Format("vec4(%s, %s, %s, 1.0)", Normalised("fetchWord & 0x7FFu", 11).c_str(),
                         Normalised("(fetchWord >> 11u) & 0x7FFu", 11).c_str(),
                         Normalised("fetchWord >> 22u", 10).c_str());
        break;
      case 17:  // k_11_11_10
        Line(Format("fetchWord = %s;", load1().c_str()));
        decoded = Format("vec4(%s, %s, %s, 1.0)", Normalised("fetchWord & 0x3FFu", 10).c_str(),
                         Normalised("(fetchWord >> 10u) & 0x7FFu", 11).c_str(),
                         Normalised("fetchWord >> 21u", 11).c_str());
        break;
      case 25:  // k_16_16
        Line(Format("fetchWord = %s;", load1().c_str()));
        decoded = Format("vec4(%s, %s, 0.0, 1.0)", Normalised("fetchWord & 0xFFFFu", 16).c_str(),
                         Normalised("fetchWord >> 16u", 16).c_str());
        break;
      case 26:  // k_16_16_16_16
        Line(Format("fetchWords = uvec2(%s, %s);", loadAt(0).c_str(), loadAt(1).c_str()));
        decoded = Format("vec4(%s, %s, %s, %s)", Normalised("fetchWords.x & 0xFFFFu", 16).c_str(),
                         Normalised("fetchWords.x >> 16u", 16).c_str(),
                         Normalised("fetchWords.y & 0xFFFFu", 16).c_str(),
                         Normalised("fetchWords.y >> 16u", 16).c_str());
        break;
      case 31:  // k_16_16_FLOAT
        Line(Format("fetchWord = %s;", load1().c_str()));
        decoded = "vec4(unpackHalf2x16(fetchWord), 0.0, 1.0)";
        break;
      case 32:  // k_16_16_16_16_FLOAT
        Line(Format("fetchWords = uvec2(%s, %s);", loadAt(0).c_str(), loadAt(1).c_str()));
        decoded = "vec4(unpackHalf2x16(fetchWords.x), unpackHalf2x16(fetchWords.y))";
        break;
      case 33:  // k_32
        Line(Format("fetchWord = %s;", load1().c_str()));
        decoded = Format("vec4(%s, 0.0, 0.0, 1.0)", Normalised("fetchWord", 32).c_str());
        break;
      case 34:  // k_32_32
        Line(Format("fetchWords = uvec2(%s, %s);", loadAt(0).c_str(), loadAt(1).c_str()));
        decoded = Format("vec4(%s, %s, 0.0, 1.0)", Normalised("fetchWords.x", 32).c_str(),
                         Normalised("fetchWords.y", 32).c_str());
        break;
      case 35:  // k_32_32_32_32
        Line(Format("fetchWords4 = uvec4(%s, %s, %s, %s);", loadAt(0).c_str(), loadAt(1).c_str(),
                    loadAt(2).c_str(), loadAt(3).c_str()));
        decoded = Format("vec4(%s, %s, %s, %s)", Normalised("fetchWords4.x", 32).c_str(),
                         Normalised("fetchWords4.y", 32).c_str(),
                         Normalised("fetchWords4.z", 32).c_str(),
                         Normalised("fetchWords4.w", 32).c_str());
        break;
      case 36:  // k_32_FLOAT
        decoded = Format("vec4(uintBitsToFloat(%s), 0.0, 0.0, 1.0)", load1().c_str());
        break;
      case 37:  // k_32_32_FLOAT
        decoded = Format("vec4(uintBitsToFloat(%s), uintBitsToFloat(%s), 0.0, 1.0)",
                         loadAt(0).c_str(), loadAt(1).c_str());
        break;
      case 57:  // k_32_32_32_FLOAT
        decoded = Format("vec4(uintBitsToFloat(%s), uintBitsToFloat(%s), uintBitsToFloat(%s), 1.0)",
                         loadAt(0).c_str(), loadAt(1).c_str(), loadAt(2).c_str());
        break;
      case 38:  // k_32_32_32_32_FLOAT
        decoded = Format(
            "vec4(uintBitsToFloat(%s), uintBitsToFloat(%s), uintBitsToFloat(%s), uintBitsToFloat(%s))",
            loadAt(0).c_str(), loadAt(1).c_str(), loadAt(2).c_str(), loadAt(3).c_str());
        break;
      default:
        out.problem = Format("vertex format %u", format);
        decoded = "vec4(0.0, 0.0, 0.0, 1.0)";
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
    const uint32_t opcode = d0 & 0x1Fu;
    const uint32_t srcReg = (d0 >> 5) & 0x3Fu;
    const uint32_t dstReg = (d0 >> 12) & 0x3Fu;
    const uint32_t slot = (d0 >> 20) & 0x1Fu;
    const bool unnormalised = ((d0 >> 25) & 1u) != 0;
    const uint32_t srcSwizzle = (d0 >> 26) & 0x3Fu;
    const uint32_t dstSwizzle = d1 & 0xFFFu;
    const bool useRegisterLod = ((d1 >> 29) & 1u) != 0;
    const bool isPredicated = ((d1 >> 31) & 1u) != 0;
    int lodBias = static_cast<int>((d2 >> 2) & 0x7Fu);
    if (lodBias >= 64) lodBias -= 128;
    auto offsetOf = [](uint32_t bits) {
      int value = static_cast<int>(bits & 0x1Fu);
      return value >= 16 ? value - 32 : value;
    };
    const int offsetX = offsetOf(d2 >> 16);
    const int offsetY = offsetOf(d2 >> 21);
    const uint32_t dimension = (d2 >> 14) & 3u;
    const bool predicateCondition = ((d2 >> 31) & 1u) != 0;

    if (opcode != 1) {
      const std::string coordinate =
          Format("r[%u].%c%c", srcReg & 0x1Fu, kComponents[srcSwizzle & 3u],
                 kComponents[(srcSwizzle >> 2) & 3u]);
      if (opcode == 24) {
        Line(Format("textureLodVal = r[%u].x;", srcReg & 0x1Fu));
      } else if (opcode == 17) {
        Line("fetched = vec4(0.0);");
      } else if (opcode == 18) {
        if (pixel) {
          Line(Format("fetched = vec4(dFdx(%s.x), dFdy(%s.x), dFdx(%s.y), dFdy(%s.y));",
                      coordinate.c_str(), coordinate.c_str(), coordinate.c_str(),
                      coordinate.c_str()));
        } else {
          Line("fetched = vec4(0.0);");
        }
      } else if (opcode == 19) {
        Line(Format("fetched = vec4(fract(%s * texSize[%u].xy - 0.5), 0.0, 0.0);",
                    coordinate.c_str(), slot));
      } else if (opcode != 25 && opcode != 26) {
        out.problem = Format("fetch opcode %u", opcode);
        return;
      }
      if (opcode == 17 || opcode == 18 || opcode == 19) FetchWrite(dstReg, dstSwizzle, "fetched");
      return;
    }

    uint32_t binding = 0;
    bool known = false;
    for (const TextureFetch& other : out.textureFetches) {
      if (other.slot == slot) {
        known = true;
        binding = other.binding;
        break;
      }
    }
    if (!known) {
      TextureFetch fetch;
      fetch.slot = slot;
      fetch.dimension = dimension;
      fetch.binding = static_cast<uint32_t>(out.textureFetches.size());
      binding = fetch.binding;
      out.textureFetches.push_back(fetch);
    }

    Line("{");
    ++indent;
    if (isPredicated) {
      Line(Format("if (p0 == %s) {", predicateCondition ? "true" : "false"));
      ++indent;
    }

    const std::string coordinate =
        Format("r[%u].%c%c%c", srcReg & 0x1Fu, kComponents[srcSwizzle & 3u],
               kComponents[(srcSwizzle >> 2) & 3u], kComponents[(srcSwizzle >> 4) & 3u]);
    const std::string texture = Format("%st%u", pixel ? "p" : "v", binding);
    const std::string size = Format("texSize[%u]", slot);

    std::string location;
    if (dimension == 3) {
      location = Format("cubeDirection(%s)", coordinate.c_str());
    } else {
      location = unnormalised ? Format("((%s).xy * %s.zw)", coordinate.c_str(), size.c_str())
                              : Format("(%s).xy", coordinate.c_str());
    }
    if (dimension != 3 && (offsetX != 0 || offsetY != 0)) {
      location = Format("(%s + vec2(%.4f, %.4f) * %s.zw)", location.c_str(), offsetX * 0.5,
                        offsetY * 0.5, size.c_str());
    }

    std::string sample;
    if (useRegisterLod || !pixel) {
      sample = Format("textureLod(%s, %s, %s)", texture.c_str(), location.c_str(),
                      useRegisterLod ? "textureLodVal" : "0.0");
    } else if (lodBias != 0) {
      sample = Format("texture(%s, %s, %.4f)", texture.c_str(), location.c_str(), lodBias / 16.0);
    } else {
      sample = Format("texture(%s, %s)", texture.c_str(), location.c_str());
    }

    Line(Format("fetched = textureAdjust(%uu, %s);", slot, sample.c_str()));
    FetchWrite(dstReg, dstSwizzle, "fetched");

    if (isPredicated) {
      --indent;
      Line("}");
    }
    --indent;
    Line("}");
  }

  void ExecBody(uint64_t instruction) {
    const uint32_t address = static_cast<uint32_t>(instruction) & 0xFFFu;
    const uint32_t count = (static_cast<uint32_t>(instruction) >> 12) & 7u;
    const uint32_t sequence = (static_cast<uint32_t>(instruction) >> 16) & 0xFFFu;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t slot = address + i;
      if (slot * 3 + 2 >= words.size()) {
        out.problem = "instruction past the end";
        return;
      }
      const uint32_t d0 = words[slot * 3 + 0];
      const uint32_t d1 = words[slot * 3 + 1];
      const uint32_t d2 = words[slot * 3 + 2];
      const bool fetch = ((sequence >> (i * 2)) & 1u) != 0;
      if (fetch) {
        if ((d0 & 0x1Fu) == 0) {
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
    return Format("((bools[%u][%u] >> %uu) & 1u)", index / 128u, (index / 32u) & 3u, index & 31u);
  }

  bool EmitStructured(const std::vector<uint64_t>& flow, uint32_t begin, uint32_t end,
                      bool inLoop) {
    auto jumpOf = [&](uint32_t i, uint32_t& target, bool& unconditional) -> bool {
      const uint64_t instruction = flow[i];
      if ((static_cast<uint32_t>(instruction >> 44) & 0xFu) != 11u) return false;
      target = static_cast<uint32_t>(instruction) & 0x1FFFu;
      unconditional = ((instruction >> 13) & 1u) != 0;
      return true;
    };
    const std::string finish = inLoop ? "break;" : "";
    uint32_t i = begin;
    while (i < end) {
      const uint64_t instruction = flow[i];
      const uint32_t opcode = static_cast<uint32_t>(instruction >> 44) & 0xFu;
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
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFFu;
          const bool condition = ((instruction >> 42) & 1u) != 0;
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
          const bool condition = ((instruction >> 42) & 1u) != 0;
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
          const bool onPredicate = ((instruction >> 14) & 1u) != 0;
          const bool condition = ((instruction >> 42) & 1u) != 0;
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFFu;
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
          const uint32_t loopEnd = static_cast<uint32_t>(instruction) & 0x1FFFu;
          const bool repeat = ((instruction >> 13) & 1u) != 0;
          const uint32_t loopId = static_cast<uint32_t>(instruction >> 16) & 0x1Fu;
          if (loopEnd <= i || loopEnd >= end) return false;
          const uint64_t last = flow[loopEnd];
          if ((static_cast<uint32_t>(last >> 44) & 0xFu) != 8u ||
              (static_cast<uint32_t>(last) & 0x1FFFu) != i) {
            return false;
          }
          for (uint32_t j = i + 1; j < loopEnd; ++j) {
            const uint32_t inside = static_cast<uint32_t>(flow[j] >> 44) & 0xFu;
            if (inside == 2 || inside == 4 || inside == 6 || inside == 14) return false;
          }
          const bool predicated = ((last >> 13) & 1u) != 0;
          const bool condition = ((last >> 42) & 1u) != 0;
          Line(Format("{ uint loopConstant%u = loops[%u][%u]; uint loopCount%u = loopConstant%u & 0xFFu;",
                      i, loopId / 4u, loopId & 3u, i, i));
          Line(Format("  int loopStep%u = int((loopConstant%u >> 16u) & 0xFFu); if (loopStep%u >= 128) loopStep%u -= 256;",
                      i, i, i, i));
          Line(Format("  int savedAL%u = aL; aL = int((loopConstant%u >> 8u) & 0xFFu);", i, i));
          Line(Format("  if (loopCount%u != 0u || %s) {", i, repeat ? "true" : "false"));
          Line(Format("  for (uint loopIndex%u = 0u; loopIndex%u < 1024u; loopIndex%u++) {",
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
      const auto savedVOrder = vertexSlotOrder;
      const auto savedVMap = vertexSlotToBinding;
      const auto savedStrides = vertexStrides;
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
      Line("for (int structuredOnce = 0; structuredOnce < 1; structuredOnce++) {");
      ++indent;
      if (EmitStructured(flow, 0, static_cast<uint32_t>(flow.size()), true)) {
        --indent;
        Line("}");
        return out.problem.empty();
      }
      body = std::move(savedBody);
      indent = savedIndent;
      vertexSlotOrder = savedVOrder;
      vertexSlotToBinding = savedVMap;
      vertexStrides = savedStrides;
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
    Line("for (uint step = 0u; step < 65536u; step++) {");
    ++indent;
    Line("switch (programCounter) {");
    for (size_t i = 0; i < flow.size(); ++i) {
      const uint64_t instruction = flow[i];
      const uint32_t opcode = static_cast<uint32_t>(instruction >> 44) & 0xFu;
      const std::string next = Format("programCounter = %d; break;", static_cast<int>(i + 1));
      const std::string finish = Format("programCounter = %d; break;", static_cast<int>(endCase));
      Line(Format("case %d: {", static_cast<int>(i)));
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
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFFu;
          const bool condition = ((instruction >> 42) & 1u) != 0;
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
          const bool condition = ((instruction >> 42) & 1u) != 0;
          Line(Format("if (p0 == %s) {", condition ? "true" : "false"));
          ++indent;
          ExecBody(instruction);
          --indent;
          Line("}");
          Line((opcode == 6 || opcode == 14) ? finish : next);
          break;
        }
        case 7: {
          const uint32_t loopEnd = static_cast<uint32_t>(instruction) & 0x1FFFu;
          const bool repeat = ((instruction >> 13) & 1u) != 0;
          const uint32_t loopId = static_cast<uint32_t>(instruction >> 16) & 0x1Fu;
          Line(Format("uint loopConstant = loops[%u][%u];", loopId / 4u, loopId & 3u));
          Line("uint count = loopConstant & 0xFFu;");
          Line(Format("if (count == 0u && !%s) { programCounter = %d; break; }",
                      repeat ? "true" : "false", static_cast<int>(loopEnd + 1)));
          Line("if (depth < 4u) { loopCount[depth] = count; loopAddress[depth] = aL; depth++; }");
          Line("aL = int((loopConstant >> 8u) & 0xFFu);");
          Line(next);
          break;
        }
        case 8: {
          const uint32_t loopStart = static_cast<uint32_t>(instruction) & 0x1FFFu;
          const uint32_t loopId = static_cast<uint32_t>(instruction >> 16) & 0x1Fu;
          const bool predicated = ((instruction >> 13) & 1u) != 0;
          const bool condition = ((instruction >> 42) & 1u) != 0;
          Line(Format("uint loopConstant = loops[%u][%u];", loopId / 4u, loopId & 3u));
          Line("int stepValue = int((loopConstant >> 16u) & 0xFFu);");
          Line("if (stepValue >= 128) stepValue -= 256;");
          Line("bool exitLoop = false;");
          if (predicated) Line(Format("if (p0 == %s) exitLoop = true;", condition ? "false" : "true"));
          Line(Format(
              "if (!exitLoop && depth > 0u && loopCount[depth - 1u] > 0u) { loopCount[depth - 1u]--; aL += stepValue; programCounter = %d; break; }",
              static_cast<int>(loopStart + 1)));
          Line("if (depth > 0u) { depth--; aL = loopAddress[depth]; }");
          Line(next);
          break;
        }
        case 11: {
          const uint32_t target = static_cast<uint32_t>(instruction) & 0x1FFFu;
          const bool unconditional = ((instruction >> 13) & 1u) != 0;
          const bool onPredicate = ((instruction >> 14) & 1u) != 0;
          const bool condition = ((instruction >> 42) & 1u) != 0;
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFFu;
          if (unconditional) {
            Line(Format("programCounter = %d; break;", static_cast<int>(target)));
          } else if (onPredicate) {
            Line(Format("if (p0 == %s) { programCounter = %d; break; }",
                        condition ? "true" : "false", static_cast<int>(target)));
          } else {
            Line(Format("if (%s == %uu) { programCounter = %d; break; }",
                        BoolConstant(boolAddress).c_str(), condition ? 1u : 0u,
                        static_cast<int>(target)));
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
    Line(Format("default: programCounter = %d; break;", static_cast<int>(endCase)));
    Line("}");
    Line(Format("if (programCounter >= %d) break;", static_cast<int>(endCase)));
    --indent;
    Line("}");
    return true;
  }
};

}  // namespace

namespace bs1::renderer::XenosGlsl {

uint64_t Version() {
  static const char* const tag = "bs1-xenos-glsl-es320-v3-deferred-resolve-clamp";
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

  std::string glsl;
  glsl.reserve(translator.body.size() + 4096);
  glsl += "#version 320 es\n";
  glsl += "precision highp float;\n";
  glsl += "precision highp int;\n";
  glsl += "// Translated from Xenos microcode by BioShock1 custom GLES 3.2 renderer.\n";
  const uint32_t floatBinding = pixel ? 1u : 0u;
  const char* floatBlockName = pixel ? "PsFloatConstants" : "VsFloatConstants";
  const char* floatArrName = pixel ? "pc" : "vc";
  glsl += Format("layout(std140, binding = %u) uniform %s { vec4 %s[256]; };\n",
                 floatBinding, floatBlockName, floatArrName);
  glsl += "layout(std140, binding = 2) uniform BoolConstants { uvec4 bools[2]; uvec4 loops[8]; };\n";
  glsl +=
      "layout(std140, binding = 3) uniform DrawConstants {\n"
      "    vec4 viewportScale;\n"
      "    vec4 viewportOffset;\n"
      "    vec4 targetSize;\n"
      "    uvec4 flags;\n"
      "    vec4 texSize[32];\n"
      "    uvec4 textureAdjustment[32];\n"
      "    vec4 pixelGen;\n"
      "    vec4 colorExpBias;\n"
      "};\n";
  glsl +=
      "uint bswap(uint v) { return (v >> 24u) | ((v >> 8u) & 0xFF00u) | ((v << 8u) & 0xFF0000u) | (v << 24u); }\n"
      "float max4(vec4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }\n"
      "vec4 blend4(vec4 mask, vec4 whenSet, vec4 whenClear) { return mix(whenClear, whenSet, mask); }\n"
      "float blend1(bool set, float whenSet, float whenClear) { return set ? whenSet : whenClear; }\n"
      "vec4 mulL(vec4 a, vec4 b) { return a * b; }\n"
      "float mulL1(float a, float b) { return a * b; }\n";
  glsl +=
      "float flipIf(float v, bool flip) { return uintBitsToFloat(floatBitsToUint(v) ^ (flip ? 0x80000000u : 0u)); }\n"
      "vec4 cubeMapX(vec3 v) { return vec4(flipIf(v.z, v.x >= 0.0), flipIf(v.y, true), v.x >= 0.0 ? 0.0 : 1.0, 2.0 * abs(v.x)); }\n"
      "vec4 cubeMapY(vec3 v) { return vec4(v.x, flipIf(v.z, v.y < 0.0), v.y >= 0.0 ? 2.0 : 3.0, 2.0 * abs(v.y)); }\n"
      "vec4 cubeMapZ(vec3 v) { return vec4(flipIf(v.x, v.z < 0.0), flipIf(v.y, true), v.z >= 0.0 ? 4.0 : 5.0, 2.0 * abs(v.z)); }\n"
      "vec4 cubeMap(vec4 a, vec4 b) {\n"
      "    return (abs(a.x) >= abs(a.y) && abs(a.x) >= abs(a.z)) ? cubeMapX(a.xyz) : (abs(a.y) >= abs(a.z)) ? cubeMapY(a.xyz) : cubeMapZ(a.xyz); }\n";
  glsl +=
      "vec3 cubeDirection(vec3 c) {\n"
      "    uint face = uint(c.z);\n"
      "    vec3 major = face == 0u ? vec3(1.0, 0.0, 0.0) : face == 1u ? vec3(-1.0, 0.0, 0.0) : face == 2u ? vec3(0.0, 1.0, 0.0)\n"
      "        : face == 3u ? vec3(0.0, -1.0, 0.0) : face == 4u ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);\n"
      "    vec3 sx = face == 0u ? vec3(0.0, 0.0, -1.0) : face == 1u ? vec3(0.0, 0.0, 1.0) : face == 5u ? vec3(-1.0, 0.0, 0.0) : vec3(1.0, 0.0, 0.0);\n"
      "    vec3 sy = face == 2u ? vec3(0.0, 0.0, 1.0) : face == 3u ? vec3(0.0, 0.0, -1.0) : vec3(0.0, -1.0, 0.0);\n"
      "    return major + sx * c.x + sy * c.y; }\n";

  // Separate texture binding ranges for VS (16..23) and PS (0..15) so linked programs never collide.
  const uint32_t texBaseUnit = pixel ? 0u : 12u;
  for (TextureFetch& fetch : out.textureFetches) {
    fetch.binding += texBaseUnit;
    const char* type = fetch.dimension == 3 ? "samplerCube" : "sampler2D";
    glsl += Format("layout(binding = %u) uniform highp %s %st%u;\n",
                   fetch.binding, type, pixel ? "p" : "v", fetch.binding - texBaseUnit);
  }

  glsl +=
      "float pickComponent(vec4 v, uint s) { return s == 0u ? v.x : s == 1u ? v.y : s == 2u ? v.z : s == 3u ? v.w : s == 4u ? 0.0 : 1.0; }\n"
      "float adjustComponent(vec4 v, uint s, uint mode) {\n"
      "    float c = pickComponent(v, s);\n"
      "    if (mode == 2u) return c * 2.0 - 1.0;\n"
      "    if (mode == 3u) return pow(abs(c), 2.2);\n"
      "    return c;\n"
      "}\n"
      "vec4 textureAdjust(uint slot, vec4 v) {\n"
      "    uvec4 adj = textureAdjustment[slot];\n"
      "    if (adj.x == 0x688u && (adj.y | adj.z | adj.w) == 0u) return v;\n"
      "    vec4 r = vec4(adjustComponent(v, adj.x & 7u, adj.y & 3u),\n"
      "                  adjustComponent(v, (adj.x >> 3u) & 7u, (adj.y >> 2u) & 3u),\n"
      "                  adjustComponent(v, (adj.x >> 6u) & 7u, (adj.y >> 4u) & 3u),\n"
      "                  adjustComponent(v, (adj.x >> 9u) & 7u, (adj.y >> 6u) & 3u));\n"
      "    if (adj.z != 0u) r *= uintBitsToFloat(adj.z ^ 0x3F800000u);\n"
      "    // Deferred resolve clamp: fast-path copies store their clamp mode here\n"
      "    // instead of running a fullscreen resolve shader.\n"
      "    if (adj.w == 1u) r = clamp(r, 0.0, 1.0);\n"
      "    else if (adj.w == 2u) r = clamp(r, -1.0, 1.0);\n"
      "    return r;\n"
      "}\n";

  // Analyze which GPRs r[0..31] are actually referenced and whether relative GPR indexing is used.
  const bool hasRelativeGpr = (translator.body.find("r[(") != std::string::npos);
  uint32_t gprMask = pixel ? 0u : 1u;  // VS always seeds r0 with gl_VertexID
  if (hasRelativeGpr) {
    gprMask = 0xFFFFFFFFu;
  } else {
    for (size_t pos = 0; (pos = translator.body.find("r[", pos)) != std::string::npos;) {
      const bool wordBefore =
          pos > 0 && (std::isalnum(static_cast<unsigned char>(translator.body[pos - 1])) ||
                      translator.body[pos - 1] == '_');
      if (!wordBefore && pos + 2 < translator.body.size() &&
          std::isdigit(static_cast<unsigned char>(translator.body[pos + 2]))) {
        const uint32_t idx =
            static_cast<uint32_t>(std::strtoul(translator.body.c_str() + pos + 2, nullptr, 10)) & 31u;
        gprMask |= (1u << idx);
      }
      pos += 2;
    }
    if (gprMask == 0u) gprMask = 1u;
    // Rewrite r[N] -> rN so the GLSL compiler sees pure SSA scalar vec4 locals instead of an array.
    std::string scalarized;
    scalarized.reserve(translator.body.size());
    for (size_t i = 0; i < translator.body.size();) {
      const bool wordBefore =
          i > 0 && (std::isalnum(static_cast<unsigned char>(translator.body[i - 1])) ||
                    translator.body[i - 1] == '_');
      if (!wordBefore && translator.body.compare(i, 2, "r[") == 0 &&
          i + 2 < translator.body.size() &&
          std::isdigit(static_cast<unsigned char>(translator.body[i + 2]))) {
        size_t end = i + 2;
        while (end < translator.body.size() &&
               std::isdigit(static_cast<unsigned char>(translator.body[end]))) {
          ++end;
        }
        if (end < translator.body.size() && translator.body[end] == ']') {
          const uint32_t idx =
              static_cast<uint32_t>(std::strtoul(translator.body.c_str() + i + 2, nullptr, 10)) & 31u;
          scalarized += Format("r%u", idx);
          i = end + 1;
          continue;
        }
      }
      scalarized += translator.body[i];
      ++i;
    }
    translator.body.swap(scalarized);
  }

  if (pixel) {
    out.colourTargets = 0;
    for (uint32_t e : translator.exportsUsed) {
      if (e < 4) out.colourTargets |= 1u << e;
    }
    if (out.colourTargets == 0) out.colourTargets = 1;

    if (!out.writesDepth && !out.usesKill) {
      glsl += "#ifndef BS1_ALPHA_TEST\nlayout(early_fragment_tests) in;\n#endif\n";
    }

    for (uint32_t i = 0; i < 16; ++i) {
      if ((gprMask & (1u << i)) != 0) {
        glsl += Format("layout(location = %u) in vec4 v_o%u;\n", i, i);
      }
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if ((out.colourTargets & (1u << i)) != 0) {
        glsl += Format("layout(location = %u) out vec4 frag_c%u;\n", i, i);
      }
    }
    glsl += "void main() {\n";
    if (hasRelativeGpr) {
      glsl += "    vec4 r[32];\n";
      for (uint32_t i = 0; i < 32; ++i) {
        glsl += i < 16 ? Format("    r[%u] = v_o%u;\n", i, i)
                       : Format("    r[%u] = vec4(0.0);\n", i);
      }
      glsl +=
          "    if (pixelGen.w != 0.0) {\n"
          "        vec2 at = floor(gl_FragCoord.xy) * pixelGen.xy;\n"
          "        vec4 generated = vec4(uintBitsToFloat(floatBitsToUint(at.x) | (gl_FrontFacing ? 0u : 0x80000000u)), at.y, 0.0, 0.0);\n"
          "        uint slot = uint(pixelGen.z);\n";
      for (uint32_t i = 0; i < 32; ++i) {
        glsl += Format("        if (slot == %uu) r[%u] = generated;\n", i, i);
      }
      glsl += "    }\n";
    } else {
      for (uint32_t i = 0; i < 32; ++i) {
        if ((gprMask & (1u << i)) == 0) continue;
        glsl += i < 16 ? Format("    vec4 r%u = v_o%u;\n", i, i)
                       : Format("    vec4 r%u = vec4(0.0);\n", i);
      }
      glsl +=
          "    if (pixelGen.w != 0.0) {\n"
          "        vec2 at = floor(gl_FragCoord.xy) * pixelGen.xy;\n"
          "        vec4 generated = vec4(uintBitsToFloat(floatBitsToUint(at.x) | (gl_FrontFacing ? 0u : 0x80000000u)), at.y, 0.0, 0.0);\n"
          "        uint slot = uint(pixelGen.z);\n";
      for (uint32_t i = 0; i < 32; ++i) {
        if ((gprMask & (1u << i)) == 0) continue;
        glsl += Format("        if (slot == %uu) r%u = generated;\n", i, i);
      }
      glsl += "    }\n";
    }
    glsl += "    vec4 oC0 = vec4(0.0), oC1 = vec4(0.0), oC2 = vec4(0.0), oC3 = vec4(0.0), oDepth4 = vec4(0.0), oUnused = vec4(0.0);\n";
  } else {
    for (uint32_t slot : translator.vertexSlotOrder) {
      const uint32_t binding = translator.vertexSlotToBinding[slot];
      glsl += Format("layout(std430, binding = %u) readonly buffer Vb%u { uint d[]; } vb%u;\n",
                     binding, binding, binding);
      VertexFetch fetch;
      fetch.slot = slot;
      fetch.binding = binding;
      if (translator.vertexStrides.count(slot)) fetch.stride = translator.vertexStrides[slot];
      out.vertexFetches.push_back(fetch);
    }
    glsl += "invariant gl_Position;\n";
    for (uint32_t i = 0; i < 16; ++i) {
      glsl += Format("layout(location = %u) out vec4 v_o%u;\n", i, i);
    }
    glsl += "void main() {\n";
    if (hasRelativeGpr) {
      glsl += "    vec4 r[32];\n";
      glsl += "    r[0] = vec4(float(gl_VertexID), 0.0, 0.0, 0.0);\n";
      for (uint32_t i = 1; i < 32; ++i) glsl += Format("    r[%u] = vec4(0.0);\n", i);
    } else {
      glsl += "    vec4 r0 = vec4(float(gl_VertexID), 0.0, 0.0, 0.0);\n";
      for (uint32_t i = 1; i < 32; ++i) {
        if ((gprMask & (1u << i)) != 0) glsl += Format("    vec4 r%u = vec4(0.0);\n", i);
      }
    }
    glsl += "    precise vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n";
    glsl += "    vec4 oPointSize = vec4(0.0, 0.0, 0.0, 1.0), oUnused = vec4(0.0, 0.0, 0.0, 1.0);\n";
    for (uint32_t i = 0; i < 16; ++i) glsl += Format("    vec4 o%u = vec4(0.0);\n", i);
    glsl +=
        "    uint fetchIndex = 0u, fetchStride = 0u, fetchWordIdx = 0u, fetchWord = 0u;\n"
        "    uvec2 fetchWords = uvec2(0u); uvec4 fetchWords4 = uvec4(0u);\n";
  }

  glsl +=
      "    vec4 fetched = vec4(0.0); float ps = 0.0; float textureLodVal = 0.0;\n"
      "    int a0 = 0; bool p0 = false;\n"
      "    int programCounter = 0; int aL = 0; uint depth = 0u; uint loopCount[4]; int loopAddress[4];\n"
      "    loopCount[0] = 0u; loopCount[1] = 0u; loopCount[2] = 0u; loopCount[3] = 0u;\n"
      "    loopAddress[0] = 0; loopAddress[1] = 0; loopAddress[2] = 0; loopAddress[3] = 0;\n";
  glsl += translator.body;

  if (pixel) {
    glsl +=
        "#ifdef BS1_ALPHA_TEST\n"
        "    { uint fn = flags.y; float ref = uintBitsToFloat(flags.z); float a = oC0.w; bool passes = true;\n"
        "      if (fn == 0u) passes = false; else if (fn == 1u) passes = a < ref; else if (fn == 2u) passes = a == ref;\n"
        "      else if (fn == 3u) passes = a <= ref; else if (fn == 4u) passes = a > ref; else if (fn == 5u) passes = a != ref;\n"
        "      else if (fn == 6u) passes = a >= ref;\n"
        "      if (!passes) discard; }\n"
        "#endif\n";
    const char* biasLane[4] = {"x", "y", "z", "w"};
    for (uint32_t i = 0; i < 4; ++i) {
      if ((out.colourTargets & (1u << i)) != 0) {
        glsl += Format("    frag_c%u = oC%u * colorExpBias.%s;\n", i, i, biasLane[i]);
      }
    }
    if (out.writesDepth) {
      glsl += "    gl_FragDepth = oDepth4.x;\n";
    }
    glsl += "}\n";
  } else {
    glsl +=
        "    { uint vte = flags.x; precise vec4 p = oPos;\n"
        "      precise float ww = ((vte & 0x400u) != 0u) ? p.w : (abs(p.w) > 1e-30 ? 1.0 / p.w : 1.0);\n"
        "      if ((vte & 0x100u) != 0u) { p.x *= ww; p.y *= ww; }\n"
        "      if ((vte & 0x200u) != 0u) { p.z *= ww; }\n"
        "      precise float xs = ((vte & 0x1u) != 0u) ? viewportScale.x : 1.0; precise float xo = ((vte & 0x2u) != 0u) ? viewportOffset.x : 0.0;\n"
        "      precise float ys = ((vte & 0x4u) != 0u) ? viewportScale.y : 1.0; precise float yo = ((vte & 0x8u) != 0u) ? viewportOffset.y : 0.0;\n"
        "      precise float zs = ((vte & 0x10u) != 0u) ? viewportScale.z : 1.0; precise float zo = ((vte & 0x20u) != 0u) ? viewportOffset.z : 0.0;\n"
        "      precise float centre = ((vte & 0x10000u) != 0u) ? 0.0 : 0.5;\n"
        "      precise float W = targetSize.x, H = targetSize.y;\n"
        "      precise float cx = p.x * (2.0 * xs / W) + ww * (2.0 * (xo + centre) / W - 1.0);\n"
        "      precise float cy = p.y * (2.0 * ys / H) + ww * (2.0 * (yo + centre) / H - 1.0);\n"
        "      precise float cz = p.z * zs + zo * ww;\n"
        "      gl_Position = vec4(cx, cy, cz, ww); }\n";
    for (uint32_t i = 0; i < 16; ++i) glsl += Format("    v_o%u = o%u;\n", i, i);
    glsl += "}\n";
    uint32_t interpolators = 0;
    for (uint32_t e : translator.exportsUsed) {
      if (e < 16) interpolators = std::max(interpolators, e + 1);
    }
    out.interpolators = interpolators;
  }

  out.glsl = std::move(glsl);
  out.ok = true;
  return out;
}

}  // namespace bs1::renderer::XenosGlsl
