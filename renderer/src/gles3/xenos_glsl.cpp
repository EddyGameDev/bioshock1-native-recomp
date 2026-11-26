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

  // Dataflow & usage tracking for dead-code / dead-interpolator elimination and hybrid FP16.
  uint8_t uncondWritten[32] = {};
  uint32_t liveInputMask = 0;
  uint32_t directPvMask = 0;
  uint32_t texCoordGprMask = 0;
  uint32_t interpFeeders[32] = {};
  uint32_t curAluInterpReads = 0;
  int controlDepth = 0;
  bool usesA0 = false;
  bool usesP0 = false;
  bool usesAL = false;
  bool usesPC = false;
  bool usesLoopStack = false;
  bool usesTexLod = false;
  bool usesFetched = false;
  bool usesPs = false;
  bool usesCubeMap = false;
  bool usesCubeDir = false;
  bool usesMax4 = false;
  bool usesBlend1 = false;
  bool usesSafeExp2 = false;
  bool usesSafeRcp = false;
  bool usesSafeRsq = false;
  bool usesHuge = false;
  bool usesMulL = false;
  bool usesMulL1 = false;

  Translator(const std::vector<uint32_t>& w, bool p) : words(w), pixel(p) {}

  void MarkTempRead(uint32_t reg, uint8_t compMask) {
    if (!pixel) return;
    if (reg & 0x40u) {
      liveInputMask |= 0xFFFFu;
      usesAL = true;
      return;
    }
    const uint32_t r = reg & 0x1Fu;
    if (r < 16u && (compMask & ~uncondWritten[r]) != 0u) {
      liveInputMask |= (1u << r);
    }
    if (r < 16u && uncondWritten[r] == 0u) {
      curAluInterpReads |= (1u << r);
    }
  }

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
      usesA0 = true;
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
      if (reg & 0x40u) {
        usesAL = true;
        base = Format("r[(%d + aL) & 31]", static_cast<int>(reg & 0x1Fu));
      } else {
        base = Format("r[%u]", reg & 0x1Fu);
      }
      uint8_t readMask = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        readMask |= static_cast<uint8_t>(1u << ((i + ((swizzle >> (i * 2)) & 3u)) & 3u));
      }
      MarkTempRead(reg, readMask);
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
                  bool relative, bool vectorTakesThree, bool readPair, bool readSingle,
                  std::string& a, std::string& b, std::string& single) {
    std::string base;
    const uint32_t laneA = vectorTakesThree ? 2u : 3u;
    const uint32_t laneB = vectorTakesThree ? 3u : 0u;
    const uint32_t compA = (laneA + ((swizzle >> (laneA * 2)) & 3u)) & 3u;
    const uint32_t compB = (laneB + ((swizzle >> (laneB * 2)) & 3u)) & 3u;
    const uint32_t compS = (3u + ((swizzle >> 6) & 3u)) & 3u;
    if (temporary) {
      if (reg & 0x40u) {
        usesAL = true;
        base = Format("r[(%d + aL) & 31]", static_cast<int>(reg & 0x1Fu));
      } else {
        base = Format("r[%u]", reg & 0x1Fu);
      }
      uint8_t readMask = 0;
      if (readPair) readMask |= static_cast<uint8_t>((1u << compA) | (1u << compB));
      if (readSingle) readMask |= static_cast<uint8_t>(1u << compS);
      if (readMask) MarkTempRead(reg, readMask);
    } else {
      base = Constant(reg, relative);
    }
    a = base + "." + kComponents[compA];
    b = base + "." + kComponents[compB];
    single = base + "." + kComponents[compS];
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

  void Alu(uint32_t d0, uint32_t d1, uint32_t d2, bool psNeededAfter) {
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

    const bool vectorKill = (vectorOpcode >= 24u && vectorOpcode <= 27u);
    const bool vectorHasSideEffect =
        (vectorKill && pixel) || (vectorOpcode >= 20u && vectorOpcode <= 23u) || vectorOpcode == 29u;
    const bool needVec = (vectorMask != 0u) || vectorHasSideEffect;

    const bool scalarIsKill = (scalarOpcode >= 35u && scalarOpcode <= 39u);
    const bool scalarHasSideEffect =
        (scalarIsKill && pixel) || scalarOpcode == 23u || scalarOpcode == 24u ||
        (scalarOpcode >= 27u && scalarOpcode <= 34u);
    const bool needScalar =
        (scalarMask != 0u) || scalarHasSideEffect ||
        (psNeededAfter && (scalarOpcode != 50u || scalarClamp));
    if (!needVec && !needScalar) return;

    curAluInterpReads = 0;
    const bool vectorTakesThree =
        vectorOpcode == 11u || (vectorOpcode >= 12u && vectorOpcode <= 14u) || vectorOpcode == 17u;
    const bool vectorUsesB =
        vectorOpcode != 8u && vectorOpcode != 9u && vectorOpcode != 10u && vectorOpcode != 19u;
    std::string a, b, c;
    if (needVec) {
      a = Source(reg1, temp1, swizzle1, negate1,
                 temp1 ? (reg1 & 0x80u) != 0 : absoluteConstants, rel1);
      if (vectorUsesB) {
        b = Source(reg2, temp2, swizzle2, negate2,
                   temp2 ? (reg2 & 0x80u) != 0 : absoluteConstants, rel2);
      }
      if (vectorTakesThree) {
        c = Source(reg3, temp3, swizzle3, negate3,
                   temp3 ? (reg3 & 0x80u) != 0 : absoluteConstants, rel3);
      }
    }
    std::string sa, sb, s1;
    if (needScalar && !(scalarOpcode >= 42u && scalarOpcode <= 47u) &&
        scalarOpcode != 33u && scalarOpcode != 50u) {
      const bool readPair =
          scalarOpcode == 0u || scalarOpcode == 2u || scalarOpcode == 4u || scalarOpcode == 5u ||
          scalarOpcode == 6u || scalarOpcode == 23u || scalarOpcode == 24u || scalarOpcode == 25u;
      ScalarPair(reg3, temp3, swizzle3, negate3, temp3 ? (reg3 & 0x80u) != 0 : absoluteConstants,
                 rel3, vectorTakesThree, readPair, !readPair, sa, sb, s1);
    }

    const bool needScope = isPredicated || (vectorMask != 0u);
    if (needScope) {
      Line("{");
      ++indent;
    }
    if (isPredicated) {
      usesP0 = true;
      Line(Format("if (p0 == %s) {", predicateCondition ? "true" : "false"));
      ++indent;
    }

    std::string vector;
    bool kill = false;
    if (needVec) {
      switch (vectorOpcode) {
        case 0:  vector = Format("(%s + %s)", a.c_str(), b.c_str()); break;
        case 1:  usesMulL = true; vector = Format("mulL(%s, %s)", a.c_str(), b.c_str()); break;
        case 2:  vector = Format("max(%s, %s)", a.c_str(), b.c_str()); break;
        case 3:  vector = Format("min(%s, %s)", a.c_str(), b.c_str()); break;
        case 4:  vector = Format("vec4(equal(%s, %s))", a.c_str(), b.c_str()); break;
        case 5:  vector = Format("vec4(greaterThan(%s, %s))", a.c_str(), b.c_str()); break;
        case 6:  vector = Format("vec4(greaterThanEqual(%s, %s))", a.c_str(), b.c_str()); break;
        case 7:  vector = Format("vec4(notEqual(%s, %s))", a.c_str(), b.c_str()); break;
        case 8:  vector = Format("fract(%s)", a.c_str()); break;
        case 9:  vector = Format("trunc(%s)", a.c_str()); break;
        case 10: vector = Format("floor(%s)", a.c_str()); break;
        case 11: usesMulL = true; vector = Format("(mulL(%s, %s) + %s)", a.c_str(), b.c_str(), c.c_str()); break;
        case 12: vector = Format("mix(%s, %s, equal(%s, vec4(0.0)))", c.c_str(), b.c_str(), a.c_str()); break;
        case 13: vector = Format("mix(%s, %s, greaterThanEqual(%s, vec4(0.0)))", c.c_str(), b.c_str(), a.c_str()); break;
        case 14: vector = Format("mix(%s, %s, greaterThan(%s, vec4(0.0)))", c.c_str(), b.c_str(), a.c_str()); break;
        case 15: usesMulL = true; vector = Format("vec4(dot(mulL(%s, %s), vec4(1.0)))", a.c_str(), b.c_str()); break;
        case 16: usesMulL = true; vector = Format("vec4(dot(mulL(%s, %s).xyz, vec3(1.0)))", a.c_str(), b.c_str()); break;
        case 17: usesMulL = true; vector = Format("vec4(dot(mulL(%s, %s).xy, vec2(1.0)) + (%s).x)", a.c_str(), b.c_str(), c.c_str()); break;
        case 18: usesCubeMap = true; vector = Format("cubeMap(%s, %s)", a.c_str(), b.c_str()); break;
        case 19: usesMax4 = true; vector = Format("vec4(max4(%s))", a.c_str()); break;
        case 20:
          usesP0 = true;
          vector = Format("vec4((%s).w == 0.0 ? 0.0 : (%s).x + 1.0)", a.c_str(), b.c_str());
          Line(Format("p0 = ((%s).w == 0.0) && ((%s).x == 0.0);", a.c_str(), b.c_str()));
          break;
        case 21:
          usesP0 = true;
          vector = Format("vec4((%s).w == 0.0 ? 0.0 : (%s).x + 1.0)", a.c_str(), b.c_str());
          Line(Format("p0 = ((%s).w == 0.0) && ((%s).x != 0.0);", a.c_str(), b.c_str()));
          break;
        case 22:
          usesP0 = true;
          vector = Format("vec4((%s).w == 0.0 ? 0.0 : (%s).x + 1.0)", a.c_str(), b.c_str());
          Line(Format("p0 = ((%s).w == 0.0) && ((%s).x > 0.0);", a.c_str(), b.c_str()));
          break;
        case 23:
          usesP0 = true;
          vector = Format("vec4((%s).w == 0.0 ? 0.0 : (%s).x + 1.0)", a.c_str(), b.c_str());
          Line(Format("p0 = ((%s).w == 0.0) && ((%s).x >= 0.0);", a.c_str(), b.c_str()));
          break;
        case 24: vector = Format("vec4(equal(%s, %s))", a.c_str(), b.c_str()); kill = true; break;
        case 25: vector = Format("vec4(greaterThan(%s, %s))", a.c_str(), b.c_str()); kill = true; break;
        case 26: vector = Format("vec4(greaterThanEqual(%s, %s))", a.c_str(), b.c_str()); kill = true; break;
        case 27: vector = Format("vec4(notEqual(%s, %s))", a.c_str(), b.c_str()); kill = true; break;
        case 28: vector = Format("vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a.c_str(), b.c_str(), a.c_str(), b.c_str()); break;
        case 29:
          usesA0 = true;
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
    }

    std::string vectorDestination;
    if (vectorMask != 0) {
      if (vectorClamp) vector = "clamp(" + vector + ", vec4(0.0), vec4(1.0))";
      vectorDestination = Destination(vectorDest, exported);
      if (pixel && !exported) {
        Line(Format("BS1_P_%u vec4 vectorValue = (%s);", vectorDest & 0x1Fu, vector.c_str()));
      } else {
        Line(Format("vec4 vectorValue = (%s);", vector.c_str()));
      }
    }

    std::string scalar;
    bool scalarKill = false;
    if (needScalar) {
      switch (scalarOpcode) {
        case 0:  scalar = Format("(%s + %s)", sa.c_str(), sb.c_str()); break;
        case 1:  usesPs = true; scalar = Format("(%s + ps)", s1.c_str()); break;
        case 2:  usesMulL1 = true; scalar = Format("mulL1(%s, %s)", sa.c_str(), sb.c_str()); break;
        case 3:  usesPs = true; usesMulL1 = true; scalar = Format("mulL1(%s, ps)", s1.c_str()); break;
        case 4:
          usesPs = true;
          usesHuge = true;
          scalar = Format("((ps == -kHuge || isnan(ps) || isnan(%s) || %s <= 0.0) ? -kHuge : %s * ps)",
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
        case 14: usesSafeExp2 = true; scalar = Format("safeExp2(%s)", s1.c_str()); break;
        case 15:
        case 16: usesHuge = true; scalar = Format("(%s > 0.0 ? log2(%s) : -kHuge)", s1.c_str(), s1.c_str()); break;
        case 17: usesHuge = true; usesSafeRcp = true; scalar = Format("(%s == 0.0 ? kHuge : safeRcp(%s))", s1.c_str(), s1.c_str()); break;
        case 18: usesSafeRcp = true; scalar = Format("(%s == 0.0 ? 0.0 : safeRcp(%s))", s1.c_str(), s1.c_str()); break;
        case 19: usesSafeRcp = true; scalar = Format("safeRcp(%s)", s1.c_str()); break;
        case 20: usesHuge = true; usesSafeRsq = true; scalar = Format("(%s <= 0.0 ? kHuge : safeRsq(%s))", s1.c_str(), s1.c_str()); break;
        case 21: usesSafeRsq = true; scalar = Format("(%s <= 0.0 ? 0.0 : safeRsq(%s))", s1.c_str(), s1.c_str()); break;
        case 22: usesSafeRsq = true; scalar = Format("safeRsq(%s)", s1.c_str()); break;
        case 23:
          usesA0 = true;
          scalar = Format("max(%s, %s)", sa.c_str(), sb.c_str());
          Line(Format("a0 = clamp(int(floor(%s + 0.5)), -256, 255);", sa.c_str()));
          break;
        case 24:
          usesA0 = true;
          scalar = Format("max(%s, %s)", sa.c_str(), sb.c_str());
          Line(Format("a0 = clamp(int(floor(%s)), -256, 255);", sa.c_str()));
          break;
        case 25: scalar = Format("(%s - %s)", sa.c_str(), sb.c_str()); break;
        case 26: usesPs = true; scalar = Format("(%s - ps)", s1.c_str()); break;
        case 27: usesP0 = true; Line(Format("p0 = (%s == 0.0);", s1.c_str())); scalar = "(p0 ? 0.0 : 1.0)"; break;
        case 28: usesP0 = true; Line(Format("p0 = (%s != 0.0);", s1.c_str())); scalar = "(p0 ? 0.0 : 1.0)"; break;
        case 29: usesP0 = true; Line(Format("p0 = (%s > 0.0);", s1.c_str())); scalar = "(p0 ? 0.0 : 1.0)"; break;
        case 30: usesP0 = true; Line(Format("p0 = (%s >= 0.0);", s1.c_str())); scalar = "(p0 ? 0.0 : 1.0)"; break;
        case 31:
          usesP0 = true;
          usesBlend1 = true;
          Line(Format("p0 = (%s == 1.0);", s1.c_str()));
          scalar = Format("(p0 ? 0.0 : blend1(%s == 0.0, 1.0, %s))", s1.c_str(), s1.c_str());
          break;
        case 32:
          usesP0 = true;
          Line(Format("p0 = (%s - 1.0 <= 0.0);", s1.c_str()));
          scalar = Format("(p0 ? 0.0 : %s - 1.0)", s1.c_str());
          break;
        case 33: usesP0 = true; usesHuge = true; Line("p0 = false;"); scalar = "kHuge"; break;
        case 34: usesP0 = true; Line(Format("p0 = (%s == 0.0);", s1.c_str())); scalar = s1; break;
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
          MarkTempRead(temporary & 0x1Fu, static_cast<uint8_t>(1u << (swizzle3 & 3u)));
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
            usesMulL1 = true;
            scalar = Format("mulL1(%s, %s)", constant.c_str(), other.c_str());
          } else {
            scalar = Format("(%s %s %s)", constant.c_str(), op, other.c_str());
          }
          break;
        }
        case 48: scalar = Format("sin(%s)", s1.c_str()); break;
        case 49: scalar = Format("cos(%s)", s1.c_str()); break;
        case 50: usesPs = true; scalar = "ps"; break;
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
      if ((scalarMask != 0u || psNeededAfter) && (scalarOpcode != 50u || scalarClamp)) {
        usesPs = true;
        Line(Format("ps = %s;", scalar.c_str()));
      }
    }

    if (vectorMask != 0) {
      const std::string m = Mask(vectorMask);
      Line(Format("%s.%s = vectorValue.%s;", vectorDestination.c_str(), m.c_str(), m.c_str()));
      if (pixel && !exported) {
        const uint32_t d = vectorDest & 0x1Fu;
        if (curAluInterpReads) interpFeeders[d] |= curAluInterpReads;
        if (controlDepth == 0 && !isPredicated) uncondWritten[d] |= static_cast<uint8_t>(vectorMask);
      }
    }
    if (scalarMask != 0) {
      usesPs = true;
      const uint32_t sDestReg = exported ? vectorDest : scalarDest;
      const std::string destination = Destination(sDestReg, exported);
      const std::string m = Mask(scalarMask);
      Line(Format("%s.%s = %s;", destination.c_str(), m.c_str(),
                  BroadcastScalar("ps", m.size()).c_str()));
      if (pixel && !exported) {
        const uint32_t d = sDestReg & 0x1Fu;
        if (curAluInterpReads) interpFeeders[d] |= curAluInterpReads;
        if (controlDepth == 0 && !isPredicated) uncondWritten[d] |= static_cast<uint8_t>(scalarMask);
      }
    }

    if (isPredicated) {
      --indent;
      Line("}");
    }
    if (needScope) {
      --indent;
      Line("}");
    }
  }

  void FetchWrite(uint32_t destReg, uint32_t swizzle, const std::string& fetched,
                  bool isPredicated = false) {
    uint8_t written = 0;
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
      written |= static_cast<uint8_t>(1u << i);
      Line(Format("r[%u].%c = %s;", destReg & 0x1Fu, kComponents[i], value.c_str()));
    }
    if (pixel && controlDepth == 0 && !isPredicated) {
      uncondWritten[destReg & 0x1Fu] |= written;
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
      usesP0 = true;
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
    const bool useTB = VertexTextureBuffers();
    // Mali's compiler rejects passing a `readonly` SSBO element directly into a
    // user function ("Function call discards 'readonly' access qualifier").
    // Stage the length and each loaded word in plain locals first, then only
    // pass those locals to helpers such as bswap()/uintBitsToFloat().
    if (useTB) {
      Line(Format("uint vbBase = vbSlice[%u].x;", binding));
      Line(Format("uint vbLen = vbSlice[%u].y;", binding));
    } else {
      Line(Format("uint vbLen = uint(%s.length()) - 1u;", buf.c_str()));
    }
    uint32_t staged = 0;
    const auto stageLoad = [&](uint32_t delta) {
      const std::string name = Format("vfw%u", staged++);
      const std::string index = Format("min(fetchWordIdx + %uu, vbLen)", delta);
      if (useTB) {
        Line(Format("uint %s = texelFetch(vb%u, int(vbBase + %s)).x;", name.c_str(), binding,
                    index.c_str()));
      } else {
        Line(Format("uint %s = %s[%s];", name.c_str(), buf.c_str(), index.c_str()));
      }
      return name;
    };
    const auto load1 = [&]() { return Format("bswap(%s)", stageLoad(0).c_str()); };
    const auto loadAt = [&](uint32_t delta) {
      return Format("bswap(%s)", stageLoad(delta).c_str());
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

    usesFetched = true;
    Line(Format("fetched = %s%s;", decoded.c_str(), scale.c_str()));
    FetchWrite(dstReg, dstSwizzle, "fetched", isPredicated);

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
      const uint32_t srcR = srcReg & 0x1Fu;
      const bool useDirectInput =
          pixel && controlDepth == 0 && srcR < 16u && uncondWritten[srcR] == 0u;
      const std::string coordinate =
          useDirectInput
              ? Format("p_v%u.%c%c", srcR, kComponents[srcSwizzle & 3u],
                       kComponents[(srcSwizzle >> 2) & 3u])
              : Format("r[%u].%c%c", srcR, kComponents[srcSwizzle & 3u],
                       kComponents[(srcSwizzle >> 2) & 3u]);
      if (opcode == 24) {
        usesTexLod = true;
        MarkTempRead(srcR, 1u);
        Line(Format("textureLodVal = r[%u].x;", srcR));
      } else if (opcode == 17) {
        usesFetched = true;
        Line("fetched = vec4(0.0);");
      } else if (opcode == 18) {
        usesFetched = true;
        if (pixel) {
          if (useDirectInput) directPvMask |= (1u << srcR);
          else texCoordGprMask |= (1u << srcR);
          MarkTempRead(srcR,
                       static_cast<uint8_t>((1u << (srcSwizzle & 3u)) |
                                            (1u << ((srcSwizzle >> 2) & 3u))));
          Line(Format("fetched = vec4(dFdx(%s.x), dFdy(%s.x), dFdx(%s.y), dFdy(%s.y));",
                      coordinate.c_str(), coordinate.c_str(), coordinate.c_str(),
                      coordinate.c_str()));
        } else {
          Line("fetched = vec4(0.0);");
        }
      } else if (opcode == 19) {
        usesFetched = true;
        if (useDirectInput) directPvMask |= (1u << srcR);
        else texCoordGprMask |= (1u << srcR);
        MarkTempRead(srcR,
                     static_cast<uint8_t>((1u << (srcSwizzle & 3u)) |
                                          (1u << ((srcSwizzle >> 2) & 3u))));
        Line(Format("fetched = vec4(fract(%s * texSize[%u].xy - 0.5), 0.0, 0.0);",
                    coordinate.c_str(), slot));
      } else if (opcode != 25 && opcode != 26) {
        out.problem = Format("fetch opcode %u", opcode);
        return;
      }
      if (opcode == 17 || opcode == 18 || opcode == 19) FetchWrite(dstReg, dstSwizzle, "fetched", false);
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

    const uint32_t srcR = srcReg & 0x1Fu;
    const bool useDirectInput =
        pixel && controlDepth == 0 && srcR < 16u && uncondWritten[srcR] == 0u;
    if (useDirectInput) {
      directPvMask |= (1u << srcR);
    } else {
      texCoordGprMask |= (1u << srcR);
    }
    uint8_t tcMask = static_cast<uint8_t>((1u << (srcSwizzle & 3u)) |
                                          (1u << ((srcSwizzle >> 2) & 3u)));
    if (dimension == 3u) tcMask |= static_cast<uint8_t>(1u << ((srcSwizzle >> 4) & 3u));
    MarkTempRead(srcR, tcMask);

    Line("{");
    ++indent;
    if (isPredicated) {
      usesP0 = true;
      Line(Format("if (p0 == %s) {", predicateCondition ? "true" : "false"));
      ++indent;
    }

    const std::string coordinate =
        useDirectInput
            ? Format("p_v%u.%c%c%c", srcR, kComponents[srcSwizzle & 3u],
                     kComponents[(srcSwizzle >> 2) & 3u], kComponents[(srcSwizzle >> 4) & 3u])
            : Format("r[%u].%c%c%c", srcR, kComponents[srcSwizzle & 3u],
                     kComponents[(srcSwizzle >> 2) & 3u], kComponents[(srcSwizzle >> 4) & 3u]);
    const std::string texture = Format("%st%u", pixel ? "p" : "v", binding);
    const std::string size = Format("texSize[%u]", slot);

    std::string location;
    if (dimension == 3) {
      usesCubeDir = true;
      location = Format("cubeDirection(%s)", coordinate.c_str());
    } else {
      const char* uvSwiz = (slot & 1u) ? "zw" : "xy";
      location = unnormalised
                     ? Format("((%s).xy * %s.zw)", coordinate.c_str(), size.c_str())
                     : Format("((%s).xy * texUvScale[%u].%s)", coordinate.c_str(), slot >> 1u, uvSwiz);
    }
    if (dimension != 3 && (offsetX != 0 || offsetY != 0)) {
      location = Format("(%s + vec2(%.4f, %.4f) * %s.zw)", location.c_str(), offsetX * 0.5,
                        offsetY * 0.5, size.c_str());
    }

    std::string sample;
    if (useRegisterLod || !pixel) {
      if (useRegisterLod) usesTexLod = true;
      sample = Format("textureLod(%s, %s, %s)", texture.c_str(), location.c_str(),
                      useRegisterLod ? "textureLodVal" : "0.0");
    } else if (lodBias != 0) {
      sample = Format("texture(%s, %s, %.4f)", texture.c_str(), location.c_str(), lodBias / 16.0);
    } else {
      sample = Format("texture(%s, %s)", texture.c_str(), location.c_str());
    }

    usesFetched = true;
    Line(Format("fetched = textureAdjust(%uu, %s);", slot, sample.c_str()));
    FetchWrite(dstReg, dstSwizzle, "fetched", isPredicated);

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
        bool psNeededAfter = true;
        if (i + 1 < count && (((sequence >> ((i + 1) * 2)) & 1u) == 0)) {
          const uint32_t nextSlot = address + i + 1;
          if (nextSlot * 3 + 2 < words.size()) {
            const uint32_t nd0 = words[nextSlot * 3 + 0];
            const uint32_t nd1 = words[nextSlot * 3 + 1];
            const bool nextPred = ((nd1 >> 28) & 1u) != 0;
            const uint32_t nextScalarOp = (nd0 >> 26) & 0x3Fu;
            const bool nextReadsPs =
                nextScalarOp == 1u || nextScalarOp == 3u || nextScalarOp == 4u ||
                nextScalarOp == 26u || nextScalarOp == 50u;
            if (!nextPred && !nextReadsPs) {
              psNeededAfter = false;
            }
          }
        }
        Alu(d0, d1, d2, psNeededAfter);
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
          ++controlDepth;
          ExecBody(instruction);
          --controlDepth;
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
          usesP0 = true;
          const bool condition = ((instruction >> 42) & 1u) != 0;
          Line(Format("if (p0 == %s) {", condition ? "true" : "false"));
          ++indent;
          ++controlDepth;
          ExecBody(instruction);
          --controlDepth;
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
            usesP0 = true;
            Line(Format("if (p0 == %s) {", condition ? "false" : "true"));
          } else {
            Line(Format("if (%s == %uu) {", BoolConstant(boolAddress).c_str(), condition ? 0u : 1u));
          }
          ++indent;
          ++controlDepth;
          uint32_t elseTarget = 0;
          bool elseUnconditional = false;
          const uint32_t last = target - 1;
          const bool ifElse = last > i && jumpOf(last, elseTarget, elseUnconditional) &&
                              elseUnconditional && elseTarget > target && elseTarget <= end;
          if (!EmitStructured(flow, i + 1, ifElse ? last : target, inLoop)) return false;
          --controlDepth;
          --indent;
          if (ifElse) {
            Line("} else {");
            ++indent;
            ++controlDepth;
            if (!EmitStructured(flow, target, elseTarget, inLoop)) return false;
            --controlDepth;
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
          usesAL = true;
          const bool predicated = ((last >> 13) & 1u) != 0;
          if (predicated) usesP0 = true;
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
          ++controlDepth;
          if (!EmitStructured(flow, i + 1, loopEnd, inLoop)) return false;
          Line(Format("aL += loopStep%u; if (loopIndex%u >= loopCount%u) break;", i, i, i));
          if (predicated) {
            Line(Format("if (p0 == %s) break;", condition ? "false" : "true"));
          }
          --controlDepth;
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

  static bool CanEmitForwardLinear(const std::vector<uint64_t>& flow) {
    const uint32_t n = static_cast<uint32_t>(flow.size());
    struct LoopSpan {
      uint32_t start;
      uint32_t end;
    };
    std::vector<LoopSpan> loops;
    std::vector<bool> matchedLoopEnd(n, false);
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t ins = flow[i];
      const uint32_t op = static_cast<uint32_t>(ins >> 44) & 0xFu;
      if (op == 9u || op == 10u) return false;
      if (op == 11u) {
        const uint32_t target = static_cast<uint32_t>(ins) & 0x1FFFu;
        if (target <= i) return false;
      } else if (op == 7u) {
        const uint32_t loopEnd = static_cast<uint32_t>(ins) & 0x1FFFu;
        if (loopEnd <= i || loopEnd >= n) return false;
        const uint64_t endIns = flow[loopEnd];
        if (((static_cast<uint32_t>(endIns >> 44) & 0xFu) != 8u) ||
            ((static_cast<uint32_t>(endIns) & 0x1FFFu) != i)) {
          return false;
        }
        if (matchedLoopEnd[loopEnd]) return false;
        matchedLoopEnd[loopEnd] = true;
        loops.push_back({i, loopEnd});
      } else if (op == 8u) {
        if (!matchedLoopEnd[i]) return false;
      }
    }
    for (const auto& L : loops) {
      for (const auto& L2 : loops) {
        if (L2.start > L.start && L2.start < L.end && L2.end >= L.end) return false;
      }
      for (uint32_t i = 0; i < n; ++i) {
        if (i >= L.start && i <= L.end) continue;
        const uint64_t ins = flow[i];
        const uint32_t op = static_cast<uint32_t>(ins >> 44) & 0xFu;
        if (op == 11u) {
          const uint32_t target = static_cast<uint32_t>(ins) & 0x1FFFu;
          if (target > L.start && target <= L.end) return false;
        }
      }
    }
    return true;
  }

  bool EmitForwardLinearRange(const std::vector<uint64_t>& flow, uint32_t begin, uint32_t end,
                              uint32_t endCase) {
    usesPC = true;
    ++controlDepth;
    uint32_t i = begin;
    while (i < end) {
      const uint64_t instruction = flow[i];
      const uint32_t opcode = static_cast<uint32_t>(instruction >> 44) & 0xFu;
      if (opcode == 0u || opcode == 12u || opcode == 15u) {
        ++i;
        continue;
      }
      if (opcode == 7u) {
        usesAL = true;
        const uint32_t loopEnd = static_cast<uint32_t>(instruction) & 0x1FFFu;
        const bool repeat = ((instruction >> 13) & 1u) != 0;
        const uint32_t loopId = static_cast<uint32_t>(instruction >> 16) & 0x1Fu;
        const uint64_t last = flow[loopEnd];
        const bool predicated = ((last >> 13) & 1u) != 0;
        if (predicated) usesP0 = true;
        const bool condition = ((last >> 42) & 1u) != 0;
        Line(Format("if (programCounter <= %d) {", static_cast<int>(i)));
        ++indent;
        Line(Format("uint loopConstant%u = loops[%u][%u]; uint loopCount%u = loopConstant%u & 0xFFu;",
                    i, loopId / 4u, loopId & 3u, i, i));
        Line(Format("int loopStep%u = int((loopConstant%u >> 16u) & 0xFFu); if (loopStep%u >= 128) loopStep%u -= 256;",
                    i, i, i, i));
        Line(Format("int savedAL%u = aL; aL = int((loopConstant%u >> 8u) & 0xFFu);", i, i));
        Line(Format("if (loopCount%u != 0u || %s) {", i, repeat ? "true" : "false"));
        ++indent;
        Line(Format("for (uint loopIndex%u = 0u; loopIndex%u < 1024u; loopIndex%u++) {", i, i, i));
        ++indent;
        Line(Format("programCounter = %d;", static_cast<int>(i + 1)));
        if (!EmitForwardLinearRange(flow, i + 1, loopEnd, endCase)) {
          --controlDepth;
          return false;
        }
        Line(Format("if (programCounter > %d) break;", static_cast<int>(loopEnd)));
        if (predicated) {
          Line(Format("if (p0 == %s) break;", condition ? "false" : "true"));
        }
        Line(Format("if (loopIndex%u >= loopCount%u) break;", i, i));
        Line(Format("aL += loopStep%u;", i));
        --indent;
        Line("}");
        --indent;
        Line("}");
        Line(Format("aL = savedAL%u;", i));
        Line(Format("if (programCounter <= %d) programCounter = %d;",
                    static_cast<int>(loopEnd), static_cast<int>(loopEnd + 1)));
        --indent;
        Line("}");
        i = loopEnd + 1;
        continue;
      }

      Line(Format("if (programCounter <= %d) {", static_cast<int>(i)));
      ++indent;
      switch (opcode) {
        case 1:
          ExecBody(instruction);
          break;
        case 2:
          ExecBody(instruction);
          Line(Format("programCounter = %d;", static_cast<int>(endCase)));
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
          if (opcode == 4) Line(Format("programCounter = %d;", static_cast<int>(endCase)));
          break;
        }
        case 5:
        case 6:
        case 13:
        case 14: {
          usesP0 = true;
          const bool condition = ((instruction >> 42) & 1u) != 0;
          Line(Format("if (p0 == %s) {", condition ? "true" : "false"));
          ++indent;
          ExecBody(instruction);
          --indent;
          Line("}");
          if (opcode == 6 || opcode == 14) {
            Line(Format("programCounter = %d;", static_cast<int>(endCase)));
          }
          break;
        }
        case 11: {
          const uint32_t target = static_cast<uint32_t>(instruction) & 0x1FFFu;
          const bool unconditional = ((instruction >> 13) & 1u) != 0;
          const bool onPredicate = ((instruction >> 14) & 1u) != 0;
          const bool condition = ((instruction >> 42) & 1u) != 0;
          const uint32_t boolAddress = static_cast<uint32_t>(instruction >> 34) & 0xFFu;
          if (unconditional) {
            Line(Format("programCounter = %d;", static_cast<int>(target)));
          } else if (onPredicate) {
            usesP0 = true;
            Line(Format("if (p0 == %s) programCounter = %d;",
                        condition ? "true" : "false", static_cast<int>(target)));
          } else {
            Line(Format("if (%s == %uu) programCounter = %d;",
                        BoolConstant(boolAddress).c_str(), condition ? 1u : 0u,
                        static_cast<int>(target)));
          }
          break;
        }
        default:
          --controlDepth;
          return false;
      }
      --indent;
      Line("}");
      ++i;
    }
    --controlDepth;
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

    uint32_t maxReachable = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(flow.size()); ++i) {
      const uint64_t f = flow[i];
      const uint32_t op = static_cast<uint32_t>(f >> 44) & 0xFu;
      if (op == 7u || op == 11u) {
        const uint32_t t = static_cast<uint32_t>(f) & 0x1FFFu;
        if (t > maxReachable) maxReachable = t;
      }
      if (op == 2u && i >= maxReachable) {
        flow.resize(i + 1);
        break;
      }
    }

    const uint32_t endCase = static_cast<uint32_t>(flow.size());
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

      auto restoreState = [&]() {
        body = savedBody;
        indent = savedIndent;
        vertexSlotOrder = savedVOrder;
        vertexSlotToBinding = savedVMap;
        vertexStrides = savedStrides;
        exportsUsed = savedExports;
        constantsRead = savedConstants;
        relativeConstants = savedRel;
        lastFullVertexSlot = 0;
        std::memset(uncondWritten, 0, sizeof(uncondWritten));
        liveInputMask = 0;
        directPvMask = 0;
        texCoordGprMask = 0;
        std::memset(interpFeeders, 0, sizeof(interpFeeders));
        curAluInterpReads = 0;
        controlDepth = 0;
        usesA0 = false;
        usesP0 = false;
        usesAL = false;
        usesPC = false;
        usesLoopStack = false;
        usesTexLod = false;
        usesFetched = false;
        usesPs = false;
        usesCubeMap = false;
        usesCubeDir = false;
        usesMax4 = false;
        usesBlend1 = false;
        usesSafeExp2 = false;
        usesSafeRcp = false;
        usesSafeRsq = false;
        usesHuge = false;
        usesMulL = false;
        usesMulL1 = false;
        out.vertexFetches = savedVtxFetches;
        out.textureFetches = savedTexFetches;
        out.writesDepth = savedDepth;
        out.usesKill = savedKill;
        out.problem = savedProblem;
      };

      body.clear();
      lastFullVertexSlot = 0;
      Line("for (int structuredOnce = 0; structuredOnce < 1; structuredOnce++) {");
      ++indent;
      if (EmitStructured(flow, 0, endCase, true)) {
        --indent;
        Line("}");
        return out.problem.empty();
      }

      restoreState();
      body.clear();
      lastFullVertexSlot = 0;
      if (CanEmitForwardLinear(flow) && EmitForwardLinearRange(flow, 0, endCase, endCase)) {
        return out.problem.empty();
      }

      out.usedInterpreter = true;  // structured and forward-linear emission failed; using fallthrough switch
      restoreState();
      usesPC = true;
      usesAL = true;
      usesLoopStack = true;
      usesP0 = true;
      ++controlDepth;
    }
    Line("for (uint step = 0u; step < 512u; step++) {");
    ++indent;
    Line("switch (programCounter) {");
    for (size_t i = 0; i < flow.size(); ++i) {
      const uint64_t instruction = flow[i];
      const uint32_t opcode = static_cast<uint32_t>(instruction >> 44) & 0xFu;
      const std::string finish = Format("programCounter = %d; break;", static_cast<int>(endCase));
      Line(Format("case %d: {", static_cast<int>(i)));
      ++indent;
      switch (opcode) {
        case 0:
        case 12:
        case 15:
          break;
        case 1:
          ExecBody(instruction);
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
          if (opcode == 4) Line(finish);
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
          if (opcode == 6 || opcode == 14) Line(finish);
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

static bool g_vertexTextureBuffers = false;
void SetVertexTextureBuffers(bool enabled) { g_vertexTextureBuffers = enabled; }
bool VertexTextureBuffers() { return g_vertexTextureBuffers; }

uint64_t Version() {
  static const char* const tag = "bs1-xenos-glsl-es320-v14-highp-mull";
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

  // Keep full IEEE FP32 highp in pixel shaders: Unreal Engine 2.5 passes world-space
  // distances/fog reciprocals (<6.1e-5 or >65504) in pc[] and GPRs that flush to 0 or
  // overflow to Inf/NaN in FP16 mediump.
  const bool useMediump = false;

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

  uint32_t liveInputs = 0u;
  uint32_t highpGprMask = 0xFFFFFFFFu;
  if (pixel) {
    liveInputs = (hasRelativeGpr || out.usedInterpreter)
                     ? ((gprMask | translator.directPvMask) & 0xFFFFu)
                     : ((translator.liveInputMask & (gprMask | translator.directPvMask)) & 0xFFFFu);
    if (useMediump) {
      if (hasRelativeGpr || out.usedInterpreter) {
        highpGprMask = 0xFFFFFFFFu;
      } else {
        highpGprMask = translator.texCoordGprMask;
        for (uint32_t d = 0; d < 32; ++d) {
          if ((translator.texCoordGprMask & (1u << d)) != 0u) {
            highpGprMask |= translator.interpFeeders[d];
          }
        }
      }
    }
  }

  std::string glsl;
  glsl.reserve(translator.body.size() + 3072);
  glsl += "#version 320 es\n";
  if (useMediump) {
    glsl +=
        "#ifdef BS1_HIGHP\n"
        "precision highp float;\n"
        "#define BS1_MP highp\n"
        "#else\n"
        "precision mediump float;\n"
        "#define BS1_MP mediump\n"
        "#endif\n";
  } else {
    glsl += "precision highp float;\n";
  }
  glsl += "precision highp int;\n";
  glsl += "// Translated from Xenos microcode by BioShock1 custom GLES 3.2 renderer.\n";
  const uint32_t floatBinding = pixel ? 1u : 0u;
  const char* floatBlockName = pixel ? "PsFloatConstants" : "VsFloatConstants";
  const char* floatArrName = pixel ? "pc" : "vc";
  glsl += Format("layout(std140, binding = %u) uniform %s { %s vec4 %s[256]; };\n",
                 floatBinding, floatBlockName, useMediump ? "BS1_MP" : "highp", floatArrName);
  glsl += "layout(std140, binding = 2) uniform BoolConstants { uvec4 bools[2]; uvec4 loops[8]; };\n";
  if (useMediump) {
    glsl +=
        "layout(std140, binding = 3) uniform DrawConstants {\n"
        "    highp vec4 viewportScale;\n"
        "    highp vec4 viewportOffset;\n"
        "    highp vec4 targetSize;\n"
        "    highp uvec4 flags;\n"
        "    highp vec4 texSize[32];\n"
        "    BS1_MP vec4 texScale[32];\n"
        "    BS1_MP vec4 texBias[32];\n"
        "    highp uvec4 texFlags[8];\n"
        "    highp vec4 pixelGen;\n"
        "    BS1_MP vec4 colorExpBias;\n"
        "    highp uvec4 vbSlice[16];\n"
        "    highp vec4 texUvScale[16];\n"
        "};\n";
  } else {
    glsl +=
        "layout(std140, binding = 3) uniform DrawConstants {\n"
        "    vec4 viewportScale;\n"
        "    vec4 viewportOffset;\n"
        "    vec4 targetSize;\n"
        "    uvec4 flags;\n"
        "    vec4 texSize[32];\n"
        "    vec4 texScale[32];\n"
        "    vec4 texBias[32];\n"
        "    uvec4 texFlags[8];\n"
        "    vec4 pixelGen;\n"
        "    vec4 colorExpBias;\n"
        "    uvec4 vbSlice[16];\n"
        "    vec4 texUvScale[16];\n"
        "};\n";
  }
  if (!pixel) {
    glsl += "uint bswap(uint v) { return (v >> 24u) | ((v >> 8u) & 0xFF00u) | ((v << 8u) & 0xFF0000u) | (v << 24u); }\n";
  }
  if (translator.usesMax4) {
    glsl += "float max4(vec4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }\n";
  }
  if (translator.usesBlend1) {
    glsl += "float blend1(bool set, float whenSet, float whenClear) { return set ? whenSet : whenClear; }\n";
  }
  if (translator.usesMulL) {
    glsl +=
        "vec4 mulL(vec4 a, vec4 b) {\n"
        "    vec4 r = a * b;\n"
        "    return mix(r, vec4(0.0), bvec4(a.x == 0.0 || b.x == 0.0, a.y == 0.0 || b.y == 0.0,\n"
        "                                   a.z == 0.0 || b.z == 0.0, a.w == 0.0 || b.w == 0.0));\n"
        "}\n";
  }
  if (translator.usesMulL1) {
    glsl += "float mulL1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : (a * b); }\n";
  }
  if (translator.usesHuge) {
    if (useMediump) {
      glsl += "#ifdef BS1_HIGHP\nconst highp float kHuge = 3.402823466e38;\n#else\nconst highp float kHuge = 65000.0;\n#endif\n";
    } else {
      glsl += "const highp float kHuge = 3.402823466e38;\n";
    }
  }
  if (translator.usesSafeExp2) {
    if (useMediump) {
      glsl += "#ifdef BS1_HIGHP\nhighp float safeExp2(highp float x) { return exp2(x); }\n#else\nhighp float safeExp2(highp float x) { return exp2(min(x, 15.9)); }\n#endif\n";
    } else {
      glsl += "highp float safeExp2(highp float x) { return exp2(x); }\n";
    }
  }
  if (translator.usesSafeRcp) {
    if (useMediump) {
      glsl += "#ifdef BS1_HIGHP\nhighp float safeRcp(highp float x) { return 1.0 / x; }\n#else\nhighp float safeRcp(highp float x) { return clamp(1.0 / x, -65000.0, 65000.0); }\n#endif\n";
    } else {
      glsl += "highp float safeRcp(highp float x) { return 1.0 / x; }\n";
    }
  }
  if (translator.usesSafeRsq) {
    if (useMediump) {
      glsl += "#ifdef BS1_HIGHP\nhighp float safeRsq(highp float x) { return inversesqrt(x); }\n#else\nhighp float safeRsq(highp float x) { return inversesqrt(max(x, 2.5e-10)); }\n#endif\n";
    } else {
      glsl += "highp float safeRsq(highp float x) { return inversesqrt(x); }\n";
    }
  }
  if (translator.usesCubeMap) {
    glsl +=
        "highp float flipIf(highp float v, bool flip) { return uintBitsToFloat(floatBitsToUint(v) ^ (flip ? 0x80000000u : 0u)); }\n"
        "vec4 cubeMapX(vec3 v) { return vec4(flipIf(v.z, v.x >= 0.0), flipIf(v.y, true), v.x >= 0.0 ? 0.0 : 1.0, 2.0 * abs(v.x)); }\n"
        "vec4 cubeMapY(vec3 v) { return vec4(v.x, flipIf(v.z, v.y < 0.0), v.y >= 0.0 ? 2.0 : 3.0, 2.0 * abs(v.y)); }\n"
        "vec4 cubeMapZ(vec3 v) { return vec4(flipIf(v.x, v.z < 0.0), flipIf(v.y, true), v.z >= 0.0 ? 4.0 : 5.0, 2.0 * abs(v.z)); }\n"
        "vec4 cubeMap(vec4 a, vec4 b) {\n"
        "    return (abs(a.x) >= abs(a.y) && abs(a.x) >= abs(a.z)) ? cubeMapX(a.xyz) : (abs(a.y) >= abs(a.z)) ? cubeMapY(a.xyz) : cubeMapZ(a.xyz); }\n";
  }
  if (translator.usesCubeDir) {
    glsl +=
        "highp vec3 cubeDirection(highp vec3 c) {\n"
        "    uint face = uint(c.z);\n"
        "    highp vec3 major = face == 0u ? vec3(1.0, 0.0, 0.0) : face == 1u ? vec3(-1.0, 0.0, 0.0) : face == 2u ? vec3(0.0, 1.0, 0.0)\n"
        "        : face == 3u ? vec3(0.0, -1.0, 0.0) : face == 4u ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);\n"
        "    highp vec3 sx = face == 0u ? vec3(0.0, 0.0, -1.0) : face == 1u ? vec3(0.0, 0.0, 1.0) : face == 5u ? vec3(-1.0, 0.0, 0.0) : vec3(1.0, 0.0, 0.0);\n"
        "    highp vec3 sy = face == 2u ? vec3(0.0, 0.0, 1.0) : face == 3u ? vec3(0.0, 0.0, -1.0) : vec3(0.0, -1.0, 0.0);\n"
        "    return major + sx * c.x + sy * c.y; }\n";
  }

  // Separate texture binding ranges for VS (12..15) and PS (0..11) so linked programs never collide.
  const uint32_t texBaseUnit = pixel ? 0u : 12u;
  for (TextureFetch& fetch : out.textureFetches) {
    fetch.binding += texBaseUnit;
    const char* type = fetch.dimension == 3 ? "samplerCube" : "sampler2D";
    glsl += Format("layout(binding = %u) uniform %s %s %st%u;\n",
                   fetch.binding, useMediump ? "BS1_MP" : "highp", type,
                   pixel ? "p" : "v", fetch.binding - texBaseUnit);
  }

  if (!out.textureFetches.empty()) {
    glsl +=
        "vec4 textureAdjust(uint slot, vec4 v) {\n"
        "    uint fl = texFlags[slot >> 2u][slot & 3u];\n"
        "    uint cm = fl & 3u;\n"
        "    if (cm == 1u) v = clamp(v, 0.0, 1.0);\n"
        "    else if (cm == 2u) v = clamp(v, -1.0, 1.0);\n"
        "    if ((fl & 0x3Cu) != 0u) {\n"
        "        vec4 a = abs(v);\n"
        "        vec4 lin = a * (a * (a * 0.305306011 + 0.682171111) + 0.012522878);\n"
        "        v = mix(v, lin, bvec4((fl & 4u) != 0u, (fl & 8u) != 0u, (fl & 16u) != 0u, (fl & 32u) != 0u));\n"
        "    }\n"
        "    return v * texScale[slot] + texBias[slot];\n"
        "}\n";
  }

  if (pixel) {
    for (uint32_t i = 0; i < 32; ++i) {
      if ((gprMask & (1u << i)) == 0) continue;
      const char* prec = (!useMediump || ((highpGprMask & (1u << i)) != 0u)) ? "highp" : "BS1_MP";
      glsl += Format("#define BS1_P_%u %s\n", i, prec);
    }

    out.colourTargets = 0;
    for (uint32_t e : translator.exportsUsed) {
      if (e < 4) out.colourTargets |= 1u << e;
    }
    if (out.colourTargets == 0) out.colourTargets = 1;

    if (!out.writesDepth && !out.usesKill) {
      glsl += "#ifndef BS1_ALPHA_TEST\nlayout(early_fragment_tests) in;\n#endif\n";
    }

    uint32_t interpolators = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      if ((liveInputs & (1u << i)) != 0u) {
        glsl += Format("layout(location = %u) in highp vec4 v_o%u;\n", i, i);
        interpolators = i + 1;
      }
    }
    out.interpolators = interpolators;
    for (uint32_t i = 0; i < 4; ++i) {
      if ((out.colourTargets & (1u << i)) != 0) {
        glsl += Format("layout(location = %u) out %s vec4 frag_c%u;\n",
                       i, useMediump ? "BS1_MP" : "highp", i);
      }
    }
    if (liveInputs != 0u) {
      glsl += "#ifndef BS1_PIXEL_GEN\n";
      for (uint32_t i = 0; i < 16; ++i) {
        if ((liveInputs & (1u << i)) != 0u) {
          glsl += Format("#define p_v%u v_o%u\n", i, i);
        }
      }
      glsl += "#endif\n";
    }
    glsl += "void main() {\n";
    if (liveInputs != 0u) {
      glsl += "#ifdef BS1_PIXEL_GEN\n";
      for (uint32_t i = 0; i < 16; ++i) {
        if ((liveInputs & (1u << i)) != 0u) {
          glsl += Format("    highp vec4 p_v%u = v_o%u;\n", i, i);
        }
      }
      glsl +=
          "    if (pixelGen.w != 0.0) {\n"
          "        highp vec2 at = floor(gl_FragCoord.xy) * pixelGen.xy;\n"
          "        highp vec4 generated = vec4(uintBitsToFloat(floatBitsToUint(at.x) | (gl_FrontFacing ? 0u : 0x80000000u)), at.y, 0.0, 0.0);\n"
          "        uint slot = uint(pixelGen.z);\n";
      for (uint32_t i = 0; i < 16; ++i) {
        if ((liveInputs & (1u << i)) != 0u) {
          glsl += Format("        if (slot == %uu) p_v%u = generated;\n", i, i);
        }
      }
      glsl += "    }\n#endif\n";
    }
    if (hasRelativeGpr) {
      glsl += "    highp vec4 r[32];\n";
      for (uint32_t i = 0; i < 32; ++i) {
        glsl += (i < 16 && (liveInputs & (1u << i)) != 0u)
                    ? Format("    r[%u] = p_v%u;\n", i, i)
                    : Format("    r[%u] = vec4(0.0);\n", i);
      }
    } else {
      for (uint32_t i = 0; i < 32; ++i) {
        if ((gprMask & (1u << i)) == 0) continue;
        const char* prec = (!useMediump || ((highpGprMask & (1u << i)) != 0u)) ? "highp" : "BS1_MP";
        glsl += (i < 16 && (liveInputs & (1u << i)) != 0u)
                    ? Format("    %s vec4 r%u = p_v%u;\n", prec, i, i)
                    : Format("    %s vec4 r%u = vec4(0.0);\n", prec, i);
      }
    }
    glsl += "    vec4 oC0 = vec4(0.0);";
    if (translator.exportsUsed.count(1)) glsl += " vec4 oC1 = vec4(0.0);";
    if (translator.exportsUsed.count(2)) glsl += " vec4 oC2 = vec4(0.0);";
    if (translator.exportsUsed.count(3)) glsl += " vec4 oC3 = vec4(0.0);";
    if (out.writesDepth) glsl += " highp vec4 oDepth4 = vec4(0.0);";
    if (translator.body.find("oUnused") != std::string::npos) glsl += " vec4 oUnused = vec4(0.0);";
    glsl += "\n";
  } else {
    for (uint32_t slot : translator.vertexSlotOrder) {
      const uint32_t binding = translator.vertexSlotToBinding[slot];
      if (VertexTextureBuffers()) {
        // Keep vertex-buffer sampler units out of the PS texture range (0..15)
        // so a buffer texture can never clobber a 2D sampler binding on Mali.
        glsl += Format("layout(binding = %u) uniform highp usamplerBuffer vb%u;\n",
                       binding + 16u, binding);
      } else {
        glsl += Format("layout(std430, binding = %u) readonly buffer Vb%u { uint d[]; } vb%u;\n",
                       binding, binding, binding);
      }
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

  if (translator.usesFetched) glsl += "    vec4 fetched = vec4(0.0);\n";
  if (translator.usesPs) glsl += "    highp float ps = 0.0;\n";
  if (translator.usesTexLod) glsl += "    highp float textureLodVal = 0.0;\n";
  if (translator.usesA0) glsl += "    int a0 = 0;\n";
  if (translator.usesP0) glsl += "    bool p0 = false;\n";
  if (translator.usesPC) glsl += "    int programCounter = 0;\n";
  if (translator.usesAL) glsl += "    int aL = 0;\n";
  if (translator.usesLoopStack) {
    glsl +=
        "    uint depth = 0u; uint loopCount[4]; int loopAddress[4];\n"
        "    loopCount[0] = 0u; loopCount[1] = 0u; loopCount[2] = 0u; loopCount[3] = 0u;\n"
        "    loopAddress[0] = 0; loopAddress[1] = 0; loopAddress[2] = 0; loopAddress[3] = 0;\n";
  }
  glsl += translator.body;

  if (pixel) {
    glsl +=
        "#ifdef BS1_ALPHA_TEST\n"
        "    { uint fn = flags.y; highp float ref = uintBitsToFloat(flags.z); highp float a = oC0.w; bool passes = true;\n"
        "      if (fn == 0u) passes = false; else if (fn == 1u) passes = a < ref; else if (fn == 2u) passes = a == ref;\n"
        "      else if (fn == 3u) passes = a <= ref; else if (fn == 4u) passes = a > ref; else if (fn == 5u) passes = a != ref;\n"
        "      else if (fn == 6u) passes = a >= ref;\n"
        "      if (!passes) discard; }\n"
        "#endif\n";
    const char* biasLane[4] = {"x", "y", "z", "w"};
    for (uint32_t i = 0; i < 4; ++i) {
      if ((out.colourTargets & (1u << i)) != 0) {
        if (useMediump) {
          glsl += Format(
              "#ifdef BS1_HIGHP\n"
              "    frag_c%u = oC%u * colorExpBias.%s;\n"
              "#else\n"
              "    frag_c%u = clamp(oC%u * colorExpBias.%s, vec4(-65000.0), vec4(65000.0));\n"
              "#endif\n",
              i, i, biasLane[i], i, i, biasLane[i]);
        } else {
          glsl += Format(
              "    { vec4 outC = oC%u * colorExpBias.%s;\n"
              "      frag_c%u = mix(outC, vec4(0.0), isnan(outC)); }\n",
              i, biasLane[i], i);
        }
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
