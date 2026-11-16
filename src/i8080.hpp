// Reverbicon 224's 8080: an instruction-level Intel 8080 (one call runs one instruction and returns its clock states), for running
// the Lexicon 224's firmware next to the DSP (src/dsp224.hpp) at the real clock ratio. Checked instruction by instruction against a
// cycle-stepped 8080 (floooh's chips i8080, as bundled with reflecticon-224) on the v4.4 firmware: test/i8080_lockstep.cpp.
// The bus is a small interface (Bus) the machine implements: memory read / write, port in / out.
#pragma once
#include <cstdint>

namespace r224 {

struct Bus {
    virtual uint8_t read(uint16_t a) = 0;
    virtual void write(uint16_t a, uint8_t v) = 0;
    virtual uint8_t in(uint8_t port) = 0;
    virtual void out(uint8_t port, uint8_t v) = 0;
    virtual ~Bus() = default;
};

struct I8080 {
    uint8_t a = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    bool fs = false, fz = false, fac = false, fp = false, fcy = false;   // S Z AC P CY
    uint16_t pc = 0, sp = 0;
    bool inte = false, halted = false;
    Bus *bus = nullptr;

    uint8_t flags() const { return uint8_t(fs << 7 | fz << 6 | fac << 4 | fp << 2 | 2 | fcy); }
    void set_flags(uint8_t f) { fs = f & 0x80; fz = f & 0x40; fac = f & 0x10; fp = f & 4; fcy = f & 1; }

    static bool parity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }
    void szp(uint8_t v) { fs = v & 0x80; fz = v == 0; fp = parity(v); }

    uint8_t rd(uint16_t x) { return bus->read(x); }
    void wr(uint16_t x, uint8_t v) { bus->write(x, v); }
    uint16_t rd16(uint16_t x) { return uint16_t(rd(x) | rd(uint16_t(x + 1)) << 8); }
    uint8_t imm() { return rd(pc++); }
    uint16_t imm16() { const uint16_t v = rd16(pc); pc += 2; return v; }
    void push(uint16_t v) { wr(--sp, uint8_t(v >> 8)); wr(--sp, uint8_t(v)); }
    uint16_t pop() { const uint16_t v = rd16(sp); sp += 2; return v; }

    uint16_t bc() const { return uint16_t(b << 8 | c); }
    uint16_t de() const { return uint16_t(d << 8 | e); }
    uint16_t hl() const { return uint16_t(h << 8 | l); }
    void set_bc(uint16_t v) { b = uint8_t(v >> 8); c = uint8_t(v); }
    void set_de(uint16_t v) { d = uint8_t(v >> 8); e = uint8_t(v); }
    void set_hl(uint16_t v) { h = uint8_t(v >> 8); l = uint8_t(v); }

    uint8_t reg(int r) {
        switch (r) { case 0: return b; case 1: return c; case 2: return d; case 3: return e; case 4: return h; case 5: return l;
                     case 6: return rd(hl()); default: return a; }
    }
    void set_reg(int r, uint8_t v) {
        switch (r) { case 0: b = v; break; case 1: c = v; break; case 2: d = v; break; case 3: e = v; break; case 4: h = v; break;
                     case 5: l = v; break; case 6: wr(hl(), v); break; default: a = v; }
    }

    // the ALU operations 0-7: ADD ADC SUB SBB ANA XRA ORA CMP
    void alu(int op, uint8_t v) {
        switch (op) {
        case 0: case 1: {
            const unsigned cin = op == 1 && fcy;
            const unsigned r = a + v + cin;
            fac = ((a & 0xf) + (v & 0xf) + cin) > 0xf;
            fcy = r > 0xff; a = uint8_t(r); szp(a); break;
        }
        case 2: case 3: case 7: {
            const unsigned bin = op == 3 && fcy;
            const unsigned r = a + uint8_t(~v) + (1 - bin);       // a - v - borrow, as the 8080's adder does it
            fac = ((a & 0xf) + (~v & 0xf) + (1 - bin)) > 0xf;
            fcy = !(r > 0xff);
            const uint8_t res = uint8_t(r);
            szp(res);
            if (op != 7) a = res;
            break;
        }
        case 4: fac = ((a | v) & 8) != 0; a &= v; fcy = false; szp(a); break;
        case 5: a ^= v; fcy = false; fac = false; szp(a); break;
        default: a |= v; fcy = false; fac = false; szp(a); break;
        }
    }
    uint8_t inr(uint8_t v) { const uint8_t r = uint8_t(v + 1); fac = (r & 0xf) == 0; szp(r); return r; }
    uint8_t dcr(uint8_t v) { const uint8_t r = uint8_t(v - 1); fac = (r & 0xf) != 0xf; szp(r); return r; }
    bool cond(int cc) const {
        switch (cc) { case 0: return !fz; case 1: return fz; case 2: return !fcy; case 3: return fcy; case 4: return !fp; case 5: return fp;
                      case 6: return !fs; default: return fs; }
    }
    void daa() {
        unsigned add = 0;
        bool cy = fcy;
        const unsigned lo = a & 0xf, hi = a >> 4;
        if (fac || lo > 9) add |= 0x06;
        if (fcy || hi > 9 || (hi >= 9 && lo > 9)) { add |= 0x60; cy = true; }
        const unsigned r = a + add;
        fac = ((a & 0xf) + (add & 0xf)) > 0xf;
        a = uint8_t(r); szp(a); fcy = cy;
    }

    // RST n as an interrupt would (the 8238 answers RST 7); returns the states taken
    int interrupt(uint8_t rst) { if (!inte) return 0; inte = false; halted = false; push(pc); pc = uint16_t(rst * 8); return 11; }

    // one instruction; returns its clock states
    int step() {
        if (halted) return 4;
        const uint8_t op = imm();
        if (op >= 0x40 && op < 0x80) {                         // MOV r,r / HLT
            if (op == 0x76) { halted = true; return 7; }
            const int dst = (op >> 3) & 7, src = op & 7;
            set_reg(dst, reg(src));
            return (dst == 6 || src == 6) ? 7 : 5;
        }
        if (op >= 0x80 && op < 0xc0) { alu((op >> 3) & 7, reg(op & 7)); return (op & 7) == 6 ? 7 : 4; }
        switch (op) {
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: return 4;   // NOP (and its aliases)
        case 0x01: set_bc(imm16()); return 10;
        case 0x11: set_de(imm16()); return 10;
        case 0x21: set_hl(imm16()); return 10;
        case 0x31: sp = imm16(); return 10;
        case 0x02: wr(bc(), a); return 7;
        case 0x12: wr(de(), a); return 7;
        case 0x0a: a = rd(bc()); return 7;
        case 0x1a: a = rd(de()); return 7;
        case 0x22: { const uint16_t x = imm16(); wr(x, l); wr(uint16_t(x + 1), h); return 16; }
        case 0x2a: { const uint16_t x = imm16(); l = rd(x); h = rd(uint16_t(x + 1)); return 16; }
        case 0x32: wr(imm16(), a); return 13;
        case 0x3a: a = rd(imm16()); return 13;
        case 0x03: set_bc(uint16_t(bc() + 1)); return 5;
        case 0x13: set_de(uint16_t(de() + 1)); return 5;
        case 0x23: set_hl(uint16_t(hl() + 1)); return 5;
        case 0x33: sp++; return 5;
        case 0x0b: set_bc(uint16_t(bc() - 1)); return 5;
        case 0x1b: set_de(uint16_t(de() - 1)); return 5;
        case 0x2b: set_hl(uint16_t(hl() - 1)); return 5;
        case 0x3b: sp--; return 5;
        case 0x09: case 0x19: case 0x29: case 0x39: {
            const uint16_t v = op == 0x09 ? bc() : op == 0x19 ? de() : op == 0x29 ? hl() : sp;
            const unsigned r = hl() + v;
            fcy = r > 0xffff; set_hl(uint16_t(r)); return 10;
        }
        case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x34: case 0x3c: {
            const int r = (op >> 3) & 7; set_reg(r, inr(reg(r))); return r == 6 ? 10 : 5;
        }
        case 0x05: case 0x0d: case 0x15: case 0x1d: case 0x25: case 0x2d: case 0x35: case 0x3d: {
            const int r = (op >> 3) & 7; set_reg(r, dcr(reg(r))); return r == 6 ? 10 : 5;
        }
        case 0x06: case 0x0e: case 0x16: case 0x1e: case 0x26: case 0x2e: case 0x36: case 0x3e: {
            const int r = (op >> 3) & 7; set_reg(r, imm()); return r == 6 ? 10 : 7;
        }
        case 0x07: fcy = a & 0x80; a = uint8_t(a << 1 | fcy); return 4;            // RLC
        case 0x0f: fcy = a & 1; a = uint8_t(a >> 1 | fcy << 7); return 4;           // RRC
        case 0x17: { const bool c0 = fcy; fcy = a & 0x80; a = uint8_t(a << 1 | c0); return 4; }   // RAL
        case 0x1f: { const bool c0 = fcy; fcy = a & 1; a = uint8_t(a >> 1 | c0 << 7); return 4; } // RAR
        case 0x27: daa(); return 4;
        case 0x2f: a = uint8_t(~a); return 4;                                        // CMA
        case 0x37: fcy = true; return 4;                                             // STC
        case 0x3f: fcy = !fcy; return 4;                                             // CMC
        case 0xc6: case 0xce: case 0xd6: case 0xde: case 0xe6: case 0xee: case 0xf6: case 0xfe: alu((op >> 3) & 7, imm()); return 7;
        case 0xc3: case 0xcb: pc = imm16(); return 10;                               // JMP
        case 0xc2: case 0xca: case 0xd2: case 0xda: case 0xe2: case 0xea: case 0xf2: case 0xfa: {
            const uint16_t t = imm16(); if (cond((op >> 3) & 7)) pc = t; return 10;
        }
        case 0xcd: case 0xdd: case 0xed: case 0xfd: { const uint16_t t = imm16(); push(pc); pc = t; return 17; }   // CALL
        case 0xc4: case 0xcc: case 0xd4: case 0xdc: case 0xe4: case 0xec: case 0xf4: case 0xfc: {
            const uint16_t t = imm16(); if (cond((op >> 3) & 7)) { push(pc); pc = t; return 17; } return 11;
        }
        case 0xc9: case 0xd9: pc = pop(); return 10;                                 // RET
        case 0xc0: case 0xc8: case 0xd0: case 0xd8: case 0xe0: case 0xe8: case 0xf0: case 0xf8:
            if (cond((op >> 3) & 7)) { pc = pop(); return 11; } return 5;
        case 0xc7: case 0xcf: case 0xd7: case 0xdf: case 0xe7: case 0xef: case 0xf7: case 0xff: push(pc); pc = uint16_t(op & 0x38); return 11;
        case 0xc5: push(bc()); return 11;
        case 0xd5: push(de()); return 11;
        case 0xe5: push(hl()); return 11;
        case 0xf5: push(uint16_t(a << 8 | flags())); return 11;
        case 0xc1: set_bc(pop()); return 10;
        case 0xd1: set_de(pop()); return 10;
        case 0xe1: set_hl(pop()); return 10;
        case 0xf1: { const uint16_t v = pop(); a = uint8_t(v >> 8); set_flags(uint8_t(v)); return 10; }
        case 0xe3: { const uint16_t v = rd16(sp); wr(sp, l); wr(uint16_t(sp + 1), h); set_hl(v); return 18; }   // XTHL
        case 0xe9: pc = hl(); return 5;                                              // PCHL
        case 0xf9: sp = hl(); return 5;                                              // SPHL
        case 0xeb: { const uint16_t t = de(); set_de(hl()); set_hl(t); return 4; }   // XCHG
        case 0xd3: bus->out(imm(), a); return 10;
        case 0xdb: a = bus->in(imm()); return 10;
        case 0xf3: inte = false; return 4;
        case 0xfb: inte = true; return 4;
        }
        return 4;
    }
};

}  // namespace r224
