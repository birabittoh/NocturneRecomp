// See src/ppc_interpreter.h. Instruction semantics intentionally mirror the
// SDK codegen's generated C++ (grep generated/*.cpp for the "// <mnemonic>"
// comments to compare) so interpreted and recompiled execution agree
// bit-for-bit; when editing, change behavior only to match codegen output.

#include "ppc_interpreter.h"

#include "generated/nocturnerecomp_init.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/logging.h>

namespace nocturne {
namespace {

struct RegFile {
  PPCRegister* g[32];
  PPCRegister* f[32];
  PPCCRRegister* cr[8];

  explicit RegFile(PPCContext& ctx) {
    PPCRegister* gg[32] = {
        &ctx.r0,  &ctx.r1,  &ctx.r2,  &ctx.r3,  &ctx.r4,  &ctx.r5,  &ctx.r6,  &ctx.r7,
        &ctx.r8,  &ctx.r9,  &ctx.r10, &ctx.r11, &ctx.r12, &ctx.r13, &ctx.r14, &ctx.r15,
        &ctx.r16, &ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23,
        &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
    PPCRegister* ff[32] = {
        &ctx.f0,  &ctx.f1,  &ctx.f2,  &ctx.f3,  &ctx.f4,  &ctx.f5,  &ctx.f6,  &ctx.f7,
        &ctx.f8,  &ctx.f9,  &ctx.f10, &ctx.f11, &ctx.f12, &ctx.f13, &ctx.f14, &ctx.f15,
        &ctx.f16, &ctx.f17, &ctx.f18, &ctx.f19, &ctx.f20, &ctx.f21, &ctx.f22, &ctx.f23,
        &ctx.f24, &ctx.f25, &ctx.f26, &ctx.f27, &ctx.f28, &ctx.f29, &ctx.f30, &ctx.f31};
    PPCCRRegister* cc[8] = {&ctx.cr0, &ctx.cr1, &ctx.cr2, &ctx.cr3,
                            &ctx.cr4, &ctx.cr5, &ctx.cr6, &ctx.cr7};
    std::memcpy(g, gg, sizeof(g));
    std::memcpy(f, ff, sizeof(f));
    std::memcpy(cr, cc, sizeof(cr));
  }

  uint8_t GetCrBit(unsigned bit) const {
    const PPCCRRegister& c = *cr[bit >> 2];
    switch (bit & 3) {
      case 0: return c.lt;
      case 1: return c.gt;
      case 2: return c.eq;
      default: return c.so;
    }
  }

  void SetCrBit(unsigned bit, uint8_t v) {
    PPCCRRegister& c = *cr[bit >> 2];
    switch (bit & 3) {
      case 0: c.lt = v; break;
      case 1: c.gt = v; break;
      case 2: c.eq = v; break;
      default: c.so = v; break;
    }
  }
};

// Field extraction (PPC big-endian bit numbering already resolved to shifts).
inline uint32_t OPCD(uint32_t i) { return i >> 26; }
inline uint32_t RT(uint32_t i) { return (i >> 21) & 31; }
inline uint32_t RA(uint32_t i) { return (i >> 16) & 31; }
inline uint32_t RB(uint32_t i) { return (i >> 11) & 31; }
inline uint32_t RC_(uint32_t i) { return (i >> 6) & 31; }  // frC in A-form
inline int32_t SIMM(uint32_t i) { return static_cast<int16_t>(i); }
inline uint32_t UIMM(uint32_t i) { return static_cast<uint16_t>(i); }
inline uint32_t XO10(uint32_t i) { return (i >> 1) & 0x3FF; }
inline uint32_t XO9(uint32_t i) { return (i >> 1) & 0x1FF; }  // arithmetic, OE stripped
inline bool RcBit(uint32_t i) { return i & 1; }
inline bool LKBit(uint32_t i) { return i & 1; }

inline uint32_t Mask32(uint32_t mb, uint32_t me) {
  uint32_t x = 0xFFFFFFFFu >> mb;
  uint32_t y = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
  return mb <= me ? (x & y) : (x | y);
}

inline uint64_t Mask64(uint32_t mb, uint32_t me) {
  uint64_t x = ~0ull >> mb;
  uint64_t y = me == 63 ? ~0ull : ~(~0ull >> (me + 1));
  return mb <= me ? (x & y) : (x | y);
}

inline uint64_t Dup32(uint32_t v) { return uint64_t(v) | (uint64_t(v) << 32); }

[[noreturn]] void Unsupported(uint32_t pc, uint32_t insn) {
  REXLOG_CRITICAL(
      "[ppc-interp] unsupported instruction {:08X} at {:08X} reached at runtime -- "
      "aborting rather than misexecuting guest code",
      insn, pc);
  std::abort();
}

void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t target, uint32_t pc) {
  PPCFunc* fn = rex::runtime::ResolveIndirectFunction(target);
  if (!fn) {
    REXLOG_CRITICAL("[ppc-interp] no host function registered for call target {:08X} (from {:08X})",
                    target, pc);
    std::abort();
  }
  fn(ctx, base);
}

// Branch decoding shared by bc/bclr/bcctr. Returns whether the branch is
// taken, updating ctr when BO says to decrement.
inline bool BranchTaken(PPCContext& ctx, const RegFile& rf, uint32_t bo, uint32_t bi) {
  bool ctr_ok = true;
  if (!(bo & 0x04)) {
    --ctx.ctr.u64;
    ctr_ok = (ctx.ctr.u32 != 0) ^ ((bo >> 1) & 1);
  }
  bool cond_ok = (bo & 0x10) || (rf.GetCrBit(bi) == ((bo >> 3) & 1));
  return ctr_ok && cond_ok;
}

enum class StepKind : uint8_t {
  kUnsupported,
  kNormal,        // falls through
  kBranch,        // b/bc: direct target, may fall through / link
  kBranchLr,      // bclr
  kBranchCtr,     // bcctr
};

// Decode-only classification used by the reachability scan. Must accept
// exactly what Execute() below implements -- keep the two in sync.
StepKind Classify(uint32_t insn) {
  switch (OPCD(insn)) {
    case 3:   // twi (codegen emits asserts as traps; executed as nop)
    case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15:
      return StepKind::kNormal;
    case 16: return StepKind::kBranch;
    case 18: return StepKind::kBranch;
    case 19:
      switch (XO10(insn)) {
        case 16: return StepKind::kBranchLr;
        case 528: return StepKind::kBranchCtr;
        case 0: case 33: case 129: case 150: case 193: case 225: case 257:
        case 289: case 417: case 449:
          return StepKind::kNormal;
        default: return StepKind::kUnsupported;
      }
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
      return StepKind::kNormal;
    case 30: {
      uint32_t xo = (insn >> 2) & 7;
      return xo <= 3 ? StepKind::kNormal : StepKind::kUnsupported;
    }
    case 31:
      if (((insn >> 2) & 0x1FF) == 413) return StepKind::kNormal;  // sradi
      switch (XO10(insn)) {
        case 0: case 4: case 19: case 20: case 23: case 24: case 26: case 28:
        case 32: case 54: case 55: case 60: case 83: case 86: case 87:
        case 119: case 124: case 144: case 146: case 150: case 151: case 183:
        case 215: case 247: case 279: case 284: case 311: case 316: case 339:
        case 343: case 375: case 407: case 412: case 439: case 444: case 467:
        case 476: case 534: case 535: case 536: case 566: case 567: case 598:
        case 599: case 631: case 662: case 663: case 695: case 727: case 759:
        case 790: case 792: case 824: case 854: case 918: case 922: case 954:
        case 982: case 983: case 986: case 1014:
          return StepKind::kNormal;
        default:
          switch (XO9(insn)) {  // OE-capable arithmetic
            case 8: case 10: case 40: case 75: case 11: case 104: case 136:
            case 138: case 202: case 234: case 235: case 266: case 457:
            case 459: case 489: case 491:
              return StepKind::kNormal;
            default: return StepKind::kUnsupported;
          }
      }
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
      return StepKind::kNormal;
    case 58: {
      uint32_t ds = insn & 3;
      return ds <= 2 ? StepKind::kNormal : StepKind::kUnsupported;
    }
    case 62: {
      uint32_t ds = insn & 3;
      return ds <= 1 ? StepKind::kNormal : StepKind::kUnsupported;
    }
    case 59:
      switch ((insn >> 1) & 0x1F) {
        case 18: case 20: case 21: case 22: case 24: case 25: case 28: case 29:
        case 30: case 31:
          return StepKind::kNormal;
        default: return StepKind::kUnsupported;
      }
    case 63:
      switch ((insn >> 1) & 0x1F) {  // A-form ops
        case 18: case 20: case 21: case 22: case 23: case 25: case 26: case 28:
        case 29: case 30: case 31:
          return StepKind::kNormal;
        default:
          switch (XO10(insn)) {
            case 0: case 12: case 14: case 15: case 40: case 72: case 136:
            case 264: case 583: case 711: case 846:
              return StepKind::kNormal;
            default: return StepKind::kUnsupported;
          }
      }
    default:
      return StepKind::kUnsupported;
  }
}

}  // namespace

bool PpcScanFunction(uint8_t* base, uint32_t start, uint32_t end, uint32_t* bad_pc,
                     uint32_t* bad_insn) {
  if (end <= start) {
    return false;
  }
  size_t count = (end - start) / 4;
  std::vector<uint8_t> visited(count, 0);
  std::vector<uint32_t> work{start};

  while (!work.empty()) {
    uint32_t pc = work.back();
    work.pop_back();
    for (;;) {
      if (pc < start || pc >= end) break;  // left the function: dispatched at runtime
      size_t idx = (pc - start) / 4;
      if (visited[idx]) break;
      visited[idx] = 1;

      uint32_t insn = REX_LOAD_U32(pc);
      StepKind kind = Classify(insn);
      if (kind == StepKind::kUnsupported) {
        if (bad_pc) *bad_pc = pc;
        if (bad_insn) *bad_insn = insn;
        return false;
      }
      if (kind == StepKind::kBranch) {
        uint32_t target;
        bool conditional;
        if (OPCD(insn) == 18) {
          int32_t li = static_cast<int32_t>(insn << 6) >> 6 & ~3;
          target = pc + li;
          conditional = false;
        } else {
          int32_t bd = static_cast<int32_t>(static_cast<int16_t>(insn & 0xFFFC));
          target = pc + bd;
          conditional = ((RT(insn) & 0x14) != 0x14);  // BO field: 1z1zz = always
        }
        if (target >= start && target < end && !visited[(target - start) / 4]) {
          work.push_back(target);
        }
        if (!conditional && !LKBit(insn)) break;  // unconditional b: no fall-through
        pc += 4;
        continue;
      }
      if (kind == StepKind::kBranchLr || kind == StepKind::kBranchCtr) {
        // bclr/bcctr: conditional forms fall through; unconditional don't.
        // bcctr targets are runtime values (jump tables) the scan can't see;
        // the interpreter aborts loudly if one lands on unsupported code.
        if ((RT(insn) & 0x14) != 0x14 || LKBit(insn)) {
          pc += 4;
          continue;
        }
        break;
      }
      pc += 4;
    }
  }
  return true;
}

void PpcInterpret(PPCContext& ctx, uint8_t* base, uint32_t start, uint32_t end) {
  RegFile rf(ctx);
  PPCRegister temp{};
  uint32_t pc = start;

  auto gpr0 = [&](uint32_t idx) -> uint64_t { return idx ? rf.g[idx]->u64 : 0; };
  auto ea_d = [&](uint32_t insn) -> uint32_t {
    return static_cast<uint32_t>(gpr0(RA(insn))) + SIMM(insn);
  };
  auto ea_x = [&](uint32_t insn) -> uint32_t {
    return static_cast<uint32_t>(gpr0(RA(insn))) + rf.g[RB(insn)]->u32;
  };
  auto record = [&](uint64_t v) { ctx.cr0.compare<int32_t>(static_cast<int32_t>(v), 0, ctx.xer); };

  for (;;) {
    uint32_t insn = REX_LOAD_U32(pc);
    uint32_t next = pc + 4;

    switch (OPCD(insn)) {
      case 3:  // twi -- codegen lowers game asserts to traps; treat as nop
        break;
      case 7:  // mulli
        rf.g[RT(insn)]->s64 = rf.g[RA(insn)]->s64 * SIMM(insn);
        break;
      case 8: {  // subfic
        uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(SIMM(insn)));
        ctx.xer.ca = rf.g[RA(insn)]->u32 <= static_cast<uint32_t>(SIMM(insn));
        rf.g[RT(insn)]->u64 = imm - rf.g[RA(insn)]->u64;
        break;
      }
      case 10:  // cmpli
        if (insn & (1u << 21)) {
          rf.cr[RT(insn) >> 2]->compare<uint64_t>(rf.g[RA(insn)]->u64, UIMM(insn), ctx.xer);
        } else {
          rf.cr[RT(insn) >> 2]->compare<uint32_t>(rf.g[RA(insn)]->u32, UIMM(insn), ctx.xer);
        }
        break;
      case 11:  // cmpi
        if (insn & (1u << 21)) {
          rf.cr[RT(insn) >> 2]->compare<int64_t>(rf.g[RA(insn)]->s64, SIMM(insn), ctx.xer);
        } else {
          rf.cr[RT(insn) >> 2]->compare<int32_t>(rf.g[RA(insn)]->s32, SIMM(insn), ctx.xer);
        }
        break;
      case 12:  // addic
      case 13:  // addic.
        ctx.xer.ca = rf.g[RA(insn)]->u32 + static_cast<uint32_t>(SIMM(insn)) <
                     rf.g[RA(insn)]->u32;
        if (SIMM(insn) < 0) {
          // Matches codegen: carry when the u32 add wraps; for negative imm
          // this is "ra.u32 > ~imm" == "ra.u32 + imm carries".
          ctx.xer.ca = rf.g[RA(insn)]->u32 > static_cast<uint32_t>(~SIMM(insn));
        }
        rf.g[RT(insn)]->s64 = rf.g[RA(insn)]->s64 + SIMM(insn);
        if (OPCD(insn) == 13) record(rf.g[RT(insn)]->u64);
        break;
      case 14:  // addi
        rf.g[RT(insn)]->s64 = static_cast<int64_t>(gpr0(RA(insn))) + SIMM(insn);
        break;
      case 15:  // addis
        rf.g[RT(insn)]->s64 =
            static_cast<int64_t>(gpr0(RA(insn))) + (static_cast<int64_t>(SIMM(insn)) << 16);
        break;

      case 16: {  // bc
        int32_t bd = static_cast<int32_t>(static_cast<int16_t>(insn & 0xFFFC));
        uint32_t target = pc + bd;
        if (BranchTaken(ctx, rf, RT(insn), RA(insn))) {
          if (LKBit(insn)) {
            ctx.lr = next;
            CallGuest(ctx, base, target, pc);
          } else if (target >= start && target < end) {
            next = target;
          } else {
            CallGuest(ctx, base, target, pc);  // conditional tail branch out
            return;
          }
        }
        break;
      }
      case 18: {  // b/bl
        int32_t li = (static_cast<int32_t>(insn << 6) >> 6) & ~3;
        uint32_t target = pc + li;
        if (LKBit(insn)) {
          ctx.lr = next;
          CallGuest(ctx, base, target, pc);
        } else if (target >= start && target < end) {
          next = target;
        } else {
          CallGuest(ctx, base, target, pc);  // tail call (incl. epilogue helpers)
          return;
        }
        break;
      }
      case 19:
        switch (XO10(insn)) {
          case 16:  // bclr
            if (BranchTaken(ctx, rf, RT(insn), RA(insn))) {
              return;
            }
            break;
          case 528: {  // bcctr
            if (BranchTaken(ctx, rf, RT(insn), RA(insn))) {
              uint32_t target = ctx.ctr.u32;
              if (LKBit(insn)) {
                ctx.lr = next;
                CallGuest(ctx, base, target, pc);
              } else if (target >= start && target < end) {
                next = target;  // jump-table dispatch within the function
              } else {
                CallGuest(ctx, base, target, pc);
                return;
              }
            }
            break;
          }
          case 0:  // mcrf
            *rf.cr[RT(insn) >> 2] = *rf.cr[RA(insn) >> 2];
            break;
          case 150:  // isync
            break;
          default: {  // cr bit ops
            uint8_t a = rf.GetCrBit(RA(insn));
            uint8_t b = rf.GetCrBit(RB(insn));
            uint8_t r;
            switch (XO10(insn)) {
              case 257: r = a & b; break;           // crand
              case 449: r = a | b; break;           // cror
              case 193: r = a ^ b; break;           // crxor
              case 225: r = !(a & b); break;        // crnand
              case 33: r = !(a | b); break;         // crnor
              case 289: r = !(a ^ b); break;        // creqv
              case 129: r = a & !b; break;          // crandc
              case 417: r = a | !b; break;          // crorc
              default: Unsupported(pc, insn);
            }
            rf.SetCrBit(RT(insn), r);
            break;
          }
        }
        break;

      case 20: {  // rlwimi
        uint32_t sh = RB(insn), mb = (insn >> 6) & 31, me = (insn >> 1) & 31;
        uint64_t mask = Mask32(mb, me);
        rf.g[RA(insn)]->u64 = (std::rotl(Dup32(rf.g[RT(insn)]->u32), sh) & mask) |
                              (rf.g[RA(insn)]->u64 & ~mask);
        if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
        break;
      }
      case 21: {  // rlwinm
        uint32_t sh = RB(insn), mb = (insn >> 6) & 31, me = (insn >> 1) & 31;
        rf.g[RA(insn)]->u64 = std::rotl(Dup32(rf.g[RT(insn)]->u32), sh) & Mask32(mb, me);
        if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
        break;
      }
      case 23: {  // rlwnm
        uint32_t mb = (insn >> 6) & 31, me = (insn >> 1) & 31;
        rf.g[RA(insn)]->u64 =
            std::rotl(Dup32(rf.g[RT(insn)]->u32), rf.g[RB(insn)]->u32 & 31) & Mask32(mb, me);
        if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
        break;
      }
      case 24:  // ori
        rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 | UIMM(insn);
        break;
      case 25:  // oris
        rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 | (static_cast<uint64_t>(UIMM(insn)) << 16);
        break;
      case 26:  // xori
        rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 ^ UIMM(insn);
        break;
      case 27:  // xoris
        rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 ^ (static_cast<uint64_t>(UIMM(insn)) << 16);
        break;
      case 28:  // andi.
        rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 & UIMM(insn);
        record(rf.g[RA(insn)]->u64);
        break;
      case 29:  // andis.
        rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 & (static_cast<uint64_t>(UIMM(insn)) << 16);
        record(rf.g[RA(insn)]->u64);
        break;

      case 30: {  // rld* (MD-form)
        uint32_t sh = RB(insn) | ((insn >> 1) & 0x20);
        uint32_t m6 = ((insn >> 6) & 0x1F) | (insn & 0x20);
        uint64_t rs = rf.g[RT(insn)]->u64;
        switch ((insn >> 2) & 7) {
          case 0:  // rldicl
            rf.g[RA(insn)]->u64 = std::rotl(rs, sh) & Mask64(m6, 63);
            break;
          case 1:  // rldicr
            rf.g[RA(insn)]->u64 = std::rotl(rs, sh) & Mask64(0, m6);
            break;
          case 2:  // rldic
            rf.g[RA(insn)]->u64 = std::rotl(rs, sh) & Mask64(m6, 63 - sh);
            break;
          case 3: {  // rldimi
            uint64_t mask = Mask64(m6, 63 - sh);
            rf.g[RA(insn)]->u64 = (std::rotl(rs, sh) & mask) | (rf.g[RA(insn)]->u64 & ~mask);
            break;
          }
          default: Unsupported(pc, insn);
        }
        if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
        break;
      }

      case 31: {
        if (((insn >> 2) & 0x1FF) == 413) {  // sradi
          uint32_t sh = RB(insn) | ((insn >> 1) & 0x20) ? (RB(insn) | (((insn >> 1) & 1) << 5)) : 0;
          sh = RB(insn) | (((insn >> 1) & 1) << 5);
          int64_t rs = rf.g[RT(insn)]->s64;
          ctx.xer.ca = (rs < 0) && sh && ((rf.g[RT(insn)]->u64 & ((1ull << sh) - 1)) != 0);
          rf.g[RA(insn)]->s64 = sh ? (rs >> sh) : rs;
          if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
          break;
        }
        uint32_t xo = XO10(insn);
        switch (xo) {
          case 0:  // cmp
            if (insn & (1u << 21)) {
              rf.cr[RT(insn) >> 2]->compare<int64_t>(rf.g[RA(insn)]->s64, rf.g[RB(insn)]->s64,
                                                     ctx.xer);
            } else {
              rf.cr[RT(insn) >> 2]->compare<int32_t>(rf.g[RA(insn)]->s32, rf.g[RB(insn)]->s32,
                                                     ctx.xer);
            }
            break;
          case 32:  // cmpl
            if (insn & (1u << 21)) {
              rf.cr[RT(insn) >> 2]->compare<uint64_t>(rf.g[RA(insn)]->u64, rf.g[RB(insn)]->u64,
                                                      ctx.xer);
            } else {
              rf.cr[RT(insn) >> 2]->compare<uint32_t>(rf.g[RA(insn)]->u32, rf.g[RB(insn)]->u32,
                                                      ctx.xer);
            }
            break;
          case 4:  // tw
            break;
          case 19: {  // mfcr
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(rf.cr[i]->raw()) << ((7 - i) * 4);
            rf.g[RT(insn)]->u64 = v;
            break;
          }
          case 144: {  // mtcrf
            uint32_t mask = (insn >> 12) & 0xFF;
            uint32_t v = rf.g[RT(insn)]->u32;
            for (int i = 0; i < 8; ++i) {
              if (mask & (0x80 >> i)) rf.cr[i]->set_raw((v >> ((7 - i) * 4)) & 0xF);
            }
            break;
          }
          case 339:  // mfspr
            switch ((RA(insn) | (RB(insn) << 5))) {
              case 1: {  // xer
                rf.g[RT(insn)]->u64 = (static_cast<uint32_t>(ctx.xer.so) << 31) |
                                      (static_cast<uint32_t>(ctx.xer.ov) << 30) |
                                      (static_cast<uint32_t>(ctx.xer.ca) << 29);
                break;
              }
              case 8: rf.g[RT(insn)]->u64 = ctx.lr; break;
              case 9: rf.g[RT(insn)]->u64 = ctx.ctr.u64; break;
              default: Unsupported(pc, insn);
            }
            break;
          case 467:  // mtspr
            switch ((RA(insn) | (RB(insn) << 5))) {
              case 1:
                ctx.xer.so = (rf.g[RT(insn)]->u32 >> 31) & 1;
                ctx.xer.ov = (rf.g[RT(insn)]->u32 >> 30) & 1;
                ctx.xer.ca = (rf.g[RT(insn)]->u32 >> 29) & 1;
                break;
              case 8: ctx.lr = rf.g[RT(insn)]->u64; break;
              case 9: ctx.ctr.u64 = rf.g[RT(insn)]->u64; break;
              default: Unsupported(pc, insn);
            }
            break;

          case 24: {  // slw
            uint8_t n = rf.g[RB(insn)]->u8;
            rf.g[RA(insn)]->u64 = (n & 0x20) ? 0 : (rf.g[RT(insn)]->u32 << (n & 0x3F));
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          }
          case 536: {  // srw
            uint8_t n = rf.g[RB(insn)]->u8;
            rf.g[RA(insn)]->u64 = (n & 0x20) ? 0 : (rf.g[RT(insn)]->u32 >> (n & 0x3F));
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          }
          case 792: {  // sraw
            uint32_t n = rf.g[RB(insn)]->u32 & 0x3F;
            if (n > 0x1F) n = 0x1F;
            int32_t rs = rf.g[RT(insn)]->s32;
            ctx.xer.ca = (rs < 0) & (((rs >> n) << n) != rs);
            rf.g[RA(insn)]->s64 = rs >> n;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          }
          case 824: {  // srawi
            uint32_t sh = RB(insn);
            int32_t rs = rf.g[RT(insn)]->s32;
            ctx.xer.ca = (rs < 0) & ((rf.g[RT(insn)]->u32 & ((1u << sh) - 1)) != 0);
            rf.g[RA(insn)]->s64 = rs >> sh;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          }
          case 26:  // cntlzw
            rf.g[RA(insn)]->u64 =
                rf.g[RT(insn)]->u32 == 0 ? 32 : __builtin_clz(rf.g[RT(insn)]->u32);
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 28:  // and
            rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 & rf.g[RB(insn)]->u64;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 60:  // andc
            rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 & ~rf.g[RB(insn)]->u64;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 124:  // nor
            rf.g[RA(insn)]->u64 = ~(rf.g[RT(insn)]->u64 | rf.g[RB(insn)]->u64);
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 284:  // eqv
            rf.g[RA(insn)]->u64 = ~(rf.g[RT(insn)]->u64 ^ rf.g[RB(insn)]->u64);
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 316:  // xor
            rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 ^ rf.g[RB(insn)]->u64;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 412:  // orc
            rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 | ~rf.g[RB(insn)]->u64;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 444:  // or
            rf.g[RA(insn)]->u64 = rf.g[RT(insn)]->u64 | rf.g[RB(insn)]->u64;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 476:  // nand
            rf.g[RA(insn)]->u64 = ~(rf.g[RT(insn)]->u64 & rf.g[RB(insn)]->u64);
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 922:  // extsh
            rf.g[RA(insn)]->s64 = rf.g[RT(insn)]->s16;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 954:  // extsb
            rf.g[RA(insn)]->s64 = rf.g[RT(insn)]->s8;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;
          case 986:  // extsw
            rf.g[RA(insn)]->s64 = rf.g[RT(insn)]->s32;
            if (RcBit(insn)) record(rf.g[RA(insn)]->u64);
            break;

          case 20: {  // lwarx
            uint32_t ea = ea_x(insn);
            ctx.reserved.u32 = *reinterpret_cast<uint32_t*>(REX_RAW_ADDR(ea));
            rf.g[RT(insn)]->u64 = __builtin_bswap32(ctx.reserved.u32);
            break;
          }
          case 150: {  // stwcx.
            uint32_t ea = ea_x(insn);
            ctx.cr0.lt = 0;
            ctx.cr0.gt = 0;
            ctx.cr0.eq = __sync_bool_compare_and_swap(
                reinterpret_cast<uint32_t*>(REX_RAW_ADDR(ea)), ctx.reserved.s32,
                __builtin_bswap32(rf.g[RT(insn)]->s32));
            ctx.cr0.so = ctx.xer.so;
            break;
          }

          case 23: rf.g[RT(insn)]->u64 = REX_LOAD_U32(ea_x(insn)); break;   // lwzx
          case 87: rf.g[RT(insn)]->u64 = REX_LOAD_U8(ea_x(insn)); break;    // lbzx
          case 279: rf.g[RT(insn)]->u64 = REX_LOAD_U16(ea_x(insn)); break;  // lhzx
          case 343:                                                          // lhax
            rf.g[RT(insn)]->s64 = static_cast<int16_t>(REX_LOAD_U16(ea_x(insn)));
            break;
          case 55: {  // lwzux
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            rf.g[RT(insn)]->u64 = REX_LOAD_U32(ea);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 119: {  // lbzux
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            rf.g[RT(insn)]->u64 = REX_LOAD_U8(ea);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 311: {  // lhzux
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            rf.g[RT(insn)]->u64 = REX_LOAD_U16(ea);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 375: {  // lhaux
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            rf.g[RT(insn)]->s64 = static_cast<int16_t>(REX_LOAD_U16(ea));
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 151: REX_STORE_U32(ea_x(insn), rf.g[RT(insn)]->u32); break;  // stwx
          case 215: REX_STORE_U8(ea_x(insn), rf.g[RT(insn)]->u8); break;    // stbx
          case 407: REX_STORE_U16(ea_x(insn), rf.g[RT(insn)]->u16); break;  // sthx
          case 183: {  // stwux
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            REX_STORE_U32(ea, rf.g[RT(insn)]->u32);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 247: {  // stbux
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            REX_STORE_U8(ea, rf.g[RT(insn)]->u8);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 439: {  // sthux
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            REX_STORE_U16(ea, rf.g[RT(insn)]->u16);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 534:  // lwbrx
            rf.g[RT(insn)]->u64 = __builtin_bswap32(REX_LOAD_U32(ea_x(insn)));
            break;
          case 790:  // lhbrx
            rf.g[RT(insn)]->u64 = __builtin_bswap16(REX_LOAD_U16(ea_x(insn)));
            break;
          case 662:  // stwbrx
            REX_STORE_U32(ea_x(insn), __builtin_bswap32(rf.g[RT(insn)]->u32));
            break;
          case 918:  // sthbrx
            REX_STORE_U16(ea_x(insn), __builtin_bswap16(rf.g[RT(insn)]->u16));
            break;

          case 535: {  // lfsx
            ctx.fpscr.disableFlushMode();
            temp.u32 = REX_LOAD_U32(ea_x(insn));
            rf.f[RT(insn)]->f64 = double(temp.f32);
            break;
          }
          case 567: {  // lfsux
            ctx.fpscr.disableFlushMode();
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            temp.u32 = REX_LOAD_U32(ea);
            rf.f[RT(insn)]->f64 = double(temp.f32);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 599:  // lfdx
            ctx.fpscr.disableFlushMode();
            rf.f[RT(insn)]->u64 = REX_LOAD_U64(ea_x(insn));
            break;
          case 631: {  // lfdux
            ctx.fpscr.disableFlushMode();
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            rf.f[RT(insn)]->u64 = REX_LOAD_U64(ea);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 663: {  // stfsx
            ctx.fpscr.disableFlushMode();
            temp.f32 = float(rf.f[RT(insn)]->f64);
            REX_STORE_U32(ea_x(insn), temp.u32);
            break;
          }
          case 695: {  // stfsux
            ctx.fpscr.disableFlushMode();
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            temp.f32 = float(rf.f[RT(insn)]->f64);
            REX_STORE_U32(ea, temp.u32);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 727:  // stfdx
            ctx.fpscr.disableFlushMode();
            REX_STORE_U64(ea_x(insn), rf.f[RT(insn)]->u64);
            break;
          case 759: {  // stfdux
            ctx.fpscr.disableFlushMode();
            uint32_t ea = rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32;
            REX_STORE_U64(ea, rf.f[RT(insn)]->u64);
            rf.g[RA(insn)]->u32 = ea;
            break;
          }
          case 983:  // stfiwx
            REX_STORE_U32(ea_x(insn), rf.f[RT(insn)]->u32);
            break;

          case 54: case 86: case 146: case 246: case 278: case 598: case 854:
          case 982: case 83:  // dcbst/dcbf/dcbtst/dcbt/sync/eieio/icbi/mfmsr-ish nops
            break;
          case 1014: {  // dcbz / dcbz128 (dcbzl)
            uint32_t ea = static_cast<uint32_t>(gpr0(RA(insn))) + rf.g[RB(insn)]->u32;
            if ((insn >> 21) & 0x10) {  // dcbz128: L field set (RT=0b10000)
              ea &= ~127u;
              std::memset(REX_RAW_ADDR(ea), 0, 128);
            } else {
              ea &= ~31u;
              std::memset(REX_RAW_ADDR(ea), 0, 32);
            }
            break;
          }

          default:
            switch (XO9(insn)) {
              case 8:  // subfc
                ctx.xer.ca = rf.g[RB(insn)]->u32 >= rf.g[RA(insn)]->u32;
                rf.g[RT(insn)]->u64 = rf.g[RB(insn)]->u64 - rf.g[RA(insn)]->u64;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 10:  // addc
                ctx.xer.ca =
                    rf.g[RA(insn)]->u32 + rf.g[RB(insn)]->u32 < rf.g[RA(insn)]->u32;
                rf.g[RT(insn)]->u64 = rf.g[RA(insn)]->u64 + rf.g[RB(insn)]->u64;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 11:  // mulhwu
                rf.g[RT(insn)]->u64 =
                    (uint64_t(rf.g[RA(insn)]->u32) * uint64_t(rf.g[RB(insn)]->u32)) >> 32;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 40:  // subf
                rf.g[RT(insn)]->s64 = rf.g[RB(insn)]->s64 - rf.g[RA(insn)]->s64;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 75:  // mulhw
                rf.g[RT(insn)]->s64 =
                    (int64_t(rf.g[RA(insn)]->s32) * int64_t(rf.g[RB(insn)]->s32)) >> 32;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 104:  // neg
                rf.g[RT(insn)]->s64 = -rf.g[RA(insn)]->s64;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 136: {  // subfe
                uint32_t a = ~rf.g[RA(insn)]->u32, b = rf.g[RB(insn)]->u32;
                uint8_t carry = (a + b < a) | (a + b + ctx.xer.ca < ctx.xer.ca);
                rf.g[RT(insn)]->u64 = ~rf.g[RA(insn)]->u64 + rf.g[RB(insn)]->u64 + ctx.xer.ca;
                ctx.xer.ca = carry;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              }
              case 138: {  // adde
                uint32_t a = rf.g[RA(insn)]->u32, b = rf.g[RB(insn)]->u32;
                uint8_t carry = (a + b < a) | (a + b + ctx.xer.ca < ctx.xer.ca);
                rf.g[RT(insn)]->u64 = rf.g[RA(insn)]->u64 + rf.g[RB(insn)]->u64 + ctx.xer.ca;
                ctx.xer.ca = carry;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              }
              case 202: {  // addze
                temp.s64 = rf.g[RA(insn)]->s64 + ctx.xer.ca;
                ctx.xer.ca = temp.u32 < rf.g[RA(insn)]->u32;
                rf.g[RT(insn)]->s64 = temp.s64;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              }
              case 234: {  // addme
                temp.s64 = rf.g[RA(insn)]->s64 + ctx.xer.ca - 1;
                ctx.xer.ca = (rf.g[RA(insn)]->u32 != 0) | ctx.xer.ca;
                rf.g[RT(insn)]->s64 = temp.s64;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              }
              case 235:  // mullw
                rf.g[RT(insn)]->s64 =
                    int64_t(rf.g[RA(insn)]->s32) * int64_t(rf.g[RB(insn)]->s32);
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 266:  // add
                rf.g[RT(insn)]->u64 = rf.g[RA(insn)]->u64 + rf.g[RB(insn)]->u64;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 457:  // divdu
                rf.g[RT(insn)]->u64 =
                    rf.g[RB(insn)]->u64 ? rf.g[RA(insn)]->u64 / rf.g[RB(insn)]->u64 : 0;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 459:  // divwu
                rf.g[RT(insn)]->u64 = uint32_t(
                    rf.g[RB(insn)]->u32 ? rf.g[RA(insn)]->u32 / rf.g[RB(insn)]->u32 : 0);
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 489:  // divd
                rf.g[RT(insn)]->s64 =
                    (rf.g[RB(insn)]->s64 &&
                     !(rf.g[RA(insn)]->s64 == INT64_MIN && rf.g[RB(insn)]->s64 == -1))
                        ? rf.g[RA(insn)]->s64 / rf.g[RB(insn)]->s64
                        : 0;
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              case 491:  // divw
                rf.g[RT(insn)]->u64 = uint32_t(
                    (rf.g[RB(insn)]->s32 &&
                     !(rf.g[RA(insn)]->s32 == INT32_MIN && rf.g[RB(insn)]->s32 == -1))
                        ? rf.g[RA(insn)]->s32 / rf.g[RB(insn)]->s32
                        : 0);
                if (RcBit(insn)) record(rf.g[RT(insn)]->u64);
                break;
              default:
                Unsupported(pc, insn);
            }
        }
        break;
      }

      case 32: rf.g[RT(insn)]->u64 = REX_LOAD_U32(ea_d(insn)); break;  // lwz
      case 33: {  // lwzu
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        rf.g[RT(insn)]->u64 = REX_LOAD_U32(ea);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 34: rf.g[RT(insn)]->u64 = REX_LOAD_U8(ea_d(insn)); break;  // lbz
      case 35: {  // lbzu
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        rf.g[RT(insn)]->u64 = REX_LOAD_U8(ea);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 36: REX_STORE_U32(ea_d(insn), rf.g[RT(insn)]->u32); break;  // stw
      case 37: {  // stwu
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        REX_STORE_U32(ea, rf.g[RT(insn)]->u32);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 38: REX_STORE_U8(ea_d(insn), rf.g[RT(insn)]->u8); break;  // stb
      case 39: {  // stbu
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        REX_STORE_U8(ea, rf.g[RT(insn)]->u8);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 40: rf.g[RT(insn)]->u64 = REX_LOAD_U16(ea_d(insn)); break;  // lhz
      case 41: {  // lhzu
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        rf.g[RT(insn)]->u64 = REX_LOAD_U16(ea);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 42:  // lha
        rf.g[RT(insn)]->s64 = static_cast<int16_t>(REX_LOAD_U16(ea_d(insn)));
        break;
      case 43: {  // lhau
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        rf.g[RT(insn)]->s64 = static_cast<int16_t>(REX_LOAD_U16(ea));
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 44: REX_STORE_U16(ea_d(insn), rf.g[RT(insn)]->u16); break;  // sth
      case 45: {  // sthu
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        REX_STORE_U16(ea, rf.g[RT(insn)]->u16);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 46: {  // lmw
        uint32_t ea = ea_d(insn);
        for (uint32_t r = RT(insn); r < 32; ++r, ea += 4) {
          rf.g[r]->u64 = REX_LOAD_U32(ea);
        }
        break;
      }
      case 47: {  // stmw
        uint32_t ea = ea_d(insn);
        for (uint32_t r = RT(insn); r < 32; ++r, ea += 4) {
          REX_STORE_U32(ea, rf.g[r]->u32);
        }
        break;
      }
      case 48: {  // lfs
        ctx.fpscr.disableFlushMode();
        temp.u32 = REX_LOAD_U32(ea_d(insn));
        rf.f[RT(insn)]->f64 = double(temp.f32);
        break;
      }
      case 49: {  // lfsu
        ctx.fpscr.disableFlushMode();
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        temp.u32 = REX_LOAD_U32(ea);
        rf.f[RT(insn)]->f64 = double(temp.f32);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 50:  // lfd
        ctx.fpscr.disableFlushMode();
        rf.f[RT(insn)]->u64 = REX_LOAD_U64(ea_d(insn));
        break;
      case 51: {  // lfdu
        ctx.fpscr.disableFlushMode();
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        rf.f[RT(insn)]->u64 = REX_LOAD_U64(ea);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 52: {  // stfs
        ctx.fpscr.disableFlushMode();
        temp.f32 = float(rf.f[RT(insn)]->f64);
        REX_STORE_U32(ea_d(insn), temp.u32);
        break;
      }
      case 53: {  // stfsu
        ctx.fpscr.disableFlushMode();
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        temp.f32 = float(rf.f[RT(insn)]->f64);
        REX_STORE_U32(ea, temp.u32);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }
      case 54:  // stfd
        ctx.fpscr.disableFlushMode();
        REX_STORE_U64(ea_d(insn), rf.f[RT(insn)]->u64);
        break;
      case 55: {  // stfdu
        ctx.fpscr.disableFlushMode();
        uint32_t ea = rf.g[RA(insn)]->u32 + SIMM(insn);
        REX_STORE_U64(ea, rf.f[RT(insn)]->u64);
        rf.g[RA(insn)]->u32 = ea;
        break;
      }

      case 58: {  // ld/ldu/lwa (DS-form)
        uint32_t ea = static_cast<uint32_t>(gpr0(RA(insn))) + (SIMM(insn) & ~3);
        switch (insn & 3) {
          case 0: rf.g[RT(insn)]->u64 = REX_LOAD_U64(ea); break;
          case 1:
            rf.g[RT(insn)]->u64 = REX_LOAD_U64(ea);
            rf.g[RA(insn)]->u32 = ea;
            break;
          case 2: rf.g[RT(insn)]->s64 = static_cast<int32_t>(REX_LOAD_U32(ea)); break;
          default: Unsupported(pc, insn);
        }
        break;
      }
      case 62: {  // std/stdu
        uint32_t ea = static_cast<uint32_t>(gpr0(RA(insn))) + (SIMM(insn) & ~3);
        REX_STORE_U64(ea, rf.g[RT(insn)]->u64);
        if ((insn & 3) == 1) rf.g[RA(insn)]->u32 = ea;
        break;
      }

      case 59: {  // FP single
        ctx.fpscr.enableFlushMode();
        double a = rf.f[RA(insn)]->f64, b = rf.f[RB(insn)]->f64, c = rf.f[RC_(insn)]->f64;
        double r;
        switch ((insn >> 1) & 0x1F) {
          case 18: r = double(float(a / b)); break;             // fdivs
          case 20: r = double(float(a - b)); break;             // fsubs
          case 21: r = double(float(a + b)); break;             // fadds
          case 22: r = double(float(std::sqrt(b))); break;      // fsqrts
          case 24: r = double(float(1.0 / b)); break;           // fres
          case 25: r = double(float(a * c)); break;             // fmuls
          case 28: r = double(float(a * c - b)); break;         // fmsubs
          case 29: r = double(float(a * c + b)); break;         // fmadds
          case 30: r = double(float(-(a * c - b))); break;      // fnmsubs
          case 31: r = double(float(-(a * c + b))); break;      // fnmadds
          default: Unsupported(pc, insn);
        }
        rf.f[RT(insn)]->f64 = r;
        break;
      }
      case 63: {  // FP double
        uint32_t aform = (insn >> 1) & 0x1F;
        double a = rf.f[RA(insn)]->f64, b = rf.f[RB(insn)]->f64, c = rf.f[RC_(insn)]->f64;
        switch (aform) {
          case 18: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = a / b; break;
          case 20: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = a - b; break;
          case 21: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = a + b; break;
          case 22: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = std::sqrt(b); break;
          case 23: rf.f[RT(insn)]->f64 = a >= 0.0 ? c : b; break;  // fsel
          case 25: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = a * c; break;
          case 26:  // frsqrte
            ctx.fpscr.enableFlushMode();
            rf.f[RT(insn)]->f64 = 1.0 / std::sqrt(b);
            break;
          case 28: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = a * c - b; break;
          case 29: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = a * c + b; break;
          case 30: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = -(a * c - b); break;
          case 31: ctx.fpscr.enableFlushMode(); rf.f[RT(insn)]->f64 = -(a * c + b); break;
          default:
            switch (XO10(insn)) {
              case 0:  // fcmpu
                rf.cr[RT(insn) >> 2]->compare(a, b);
                break;
              case 12:  // frsp
                ctx.fpscr.enableFlushMode();
                rf.f[RT(insn)]->f64 = double(float(b));
                break;
              case 14:  // fctiw
                rf.f[RT(insn)]->s64 = std::isnan(b)               ? int64_t(0x80000000u)
                                      : (b > double(INT32_MAX))   ? INT32_MAX
                                      : simde_mm_cvtsd_si32(simde_mm_load_sd(&b));
                break;
              case 15:  // fctiwz
                rf.f[RT(insn)]->s64 = std::isnan(b)               ? int64_t(0x80000000u)
                                      : (b > double(INT32_MAX))   ? INT32_MAX
                                      : simde_mm_cvttsd_si32(simde_mm_load_sd(&b));
                break;
              case 40: rf.f[RT(insn)]->f64 = -b; break;                       // fneg
              case 72: rf.f[RT(insn)]->f64 = b; break;                        // fmr
              case 136: rf.f[RT(insn)]->f64 = -std::fabs(b); break;           // fnabs
              case 264: rf.f[RT(insn)]->f64 = std::fabs(b); break;            // fabs
              case 583:  // mffs
                rf.f[RT(insn)]->u64 = ctx.fpscr.loadFromHost();
                break;
              case 711:  // mtfsf
                ctx.fpscr.storeFromGuest(rf.f[RB(insn)]->u32);
                break;
              case 846:  // fcfid
                ctx.fpscr.enableFlushMode();
                rf.f[RT(insn)]->f64 = double(rf.f[RB(insn)]->s64);
                break;
              default: Unsupported(pc, insn);
            }
        }
        break;
      }

      default:
        Unsupported(pc, insn);
    }

    pc = next;
    if (pc < start || pc >= end) {
      REXLOG_CRITICAL("[ppc-interp] fell off function bounds at {:08X} (function {:08X}-{:08X})",
                      pc, start, end);
      std::abort();
    }
  }
}

}  // namespace nocturne
