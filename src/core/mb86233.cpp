// Fujitsu MB86233 (TGP), the Model 1 geometry coprocessor. MAME's core
// (src/devices/cpu/mb86233/mb86233.cpp, BSD-3-Clause, copyright-holders: Olivier Galibert),
// without the device framework: memory goes through mb86233_bus.
#include "mb86233.h"
#include <cstdarg>

static void mb_log(const char *fmt, ...)
{
	if (!getenv("MB86233_LOG")) return;
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

void mb86233_device::reset()
{
	m_pc = 0;
	m_ppc = 0;
	m_st = F_ZRC|F_ZRD|F_ZX0|F_ZX1|F_ZX2|F_ZC0|F_ZC1;
	m_sp = 0;

	m_a = 0;
	m_b = 0;
	m_d = 0;
	m_p = 0;
	m_r = 1;
	m_rpc = 1;
	m_c0 = 1;
	m_c1 = 1;
	m_b0 = 0;
	m_b1 = 0;
	m_x0 = 0;
	m_x1 = 0;
	m_i0 = 0;
	m_i1 = 0;
	m_sft = 0;
	m_vsm = 0;
	m_vsmr = 7;
	m_mask = 0;
	m_m = 1;

	m_alu_stmask = 0;
	m_alu_stset = 0;
	m_alu_r1 = 0;
	m_alu_r2 = 0;

	std::fill(std::begin(m_pcs), std::end(m_pcs), 0);

	m_stall = false;
	m_prog_dirty = true;
}

inline u32 mb86233_device::set_exp(u32 val, u32 exp)
{
	return (val & 0x807fffff) | ((exp & 0xff) << 23);
}

inline u32 mb86233_device::set_mant(u32 val, u32 mant)
{
	return (val & 0x07f800000) | ((mant & 0x00800000) << 8) | (mant & 0x007fffff);
}

inline u32 mb86233_device::get_exp(u32 val)
{
	return (val >> 23) & 0xff;
}

inline u32 mb86233_device::get_mant(u32 val)
{
	return val & 0x80000000 ? val | 0x7f800000 : val & 0x807fffff;
}

inline void mb86233_device::pcs_push()
{
	m_pcs[3] = m_pcs[2];
	m_pcs[2] = m_pcs[1];
	m_pcs[1] = m_pcs[0];
	m_pcs[0] = m_pc;
}

inline void mb86233_device::pcs_pop()
{
	m_pc = m_pcs[0];
	m_pcs[0] = m_pcs[1];
	m_pcs[1] = m_pcs[2];
	m_pcs[2] = m_pcs[3];
}

void mb86233_device::stset_set_sz_int(u32 val)
{
	m_alu_stset = val ? (val & 0x80000000 ? F_SGD : 0) : F_ZRD;
}

void mb86233_device::stset_set_sz_fp(u32 val)
{
	m_alu_stset = (val & 0x7fffffff) ? (val & 0x80000000 ? F_SGD : 0) : F_ZRD;
}

void mb86233_device::alu_pre(u32 alu)
{
	switch(alu) {
	case 0x00: break; // no alu

	case 0x01: {
		// andd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d & m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x02: {
		// orad
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d | m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x03: {
		// eord
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d ^ m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x04: {
		// notd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = ~m_d;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x05: {
		// fcpd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		u32 r = f2u(u2f(m_d) - u2f(m_a));
		stset_set_sz_fp(r);
		break;
	}

	case 0x06: {
		// fadd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x07: {
		// fsbd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) - u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x08: {
		// fml
		m_alu_stmask = 0;
		m_alu_r1 = f2u(u2f(m_a) * u2f(m_b));
		m_alu_stset = 0;
		break;
	}

	case 0x09: {
		// fmsd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0a: {
		// fmrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) - u2f(m_p));
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0b: {
		// fabd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d & 0x7fffffff;
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0c: {
		// fsmd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0d: {
		// fspd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_p;
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0e: {
		// cxfd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(s32(m_d));
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x0f: {
		// cfxd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		switch((m_m >> 1) & 3) {
		case 0: m_alu_r1 = f2s32(roundf(u2f(m_d))); break;   // (as x86-64 on every machine: mame_shim.h)
		case 1: m_alu_r1 = f2s32(ceilf(u2f(m_d))); break;
		case 2: m_alu_r1 = f2s32(floorf(u2f(m_d))); break;
		case 3: m_alu_r1 = f2s32(u2f(m_d)); break;
		}
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x10: {
		// fdvd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) / u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x11: {
		// fned
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d ? m_d ^ 0x80000000 : 0;
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x13: {
		// d = b + a
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_b) + u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x14: {
		// d = b - a
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_b) - u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x16: {
		// lsrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d >> (m_sft & 31);   // (vf1-recomp: a count of 32 or more, from a state from elsewhere, as x86 and Arm shift: undefined in C++)
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x17: {
		// lsld
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d << (m_sft & 31);
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x18: {
		// asrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = s32(m_d) >> (m_sft & 31);
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x19: {
		// asld
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = u32(s32(m_d)) << (m_sft & 31);
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x1a: {
		// addd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d + m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x1b: {
		// subd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d - m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	default:
		mb_log("unhandled alu pre %02x\n", alu);
		break;
	}
}

void mb86233_device::alu_update_st()
{
	m_st = (m_st & ~m_alu_stmask) | m_alu_stset;
}

void mb86233_device::alu_post_1(u32 alu)
{
	// integer alu post ops
	switch(alu) {
	case 0x01: case 0x02: case 0x03: case 0x04:
	case 0x0e: case 0x0f: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1a: case 0x1b:
		// d update
		m_d = m_alu_r1;
		alu_update_st();
		break;

	default:
		break;
	}
}

void mb86233_device::alu_post_2(u32 alu)
{
	// floating point alu post ops
	// assume each one takes 2 cycles
	switch (alu) {
	case 0x05:
		// flags only
		alu_update_st();
		m_icount--;
		break;

	case 0x06: case 0x07: case 0x0b: case 0x0c:
	case 0x10: case 0x11: case 0x13: case 0x14:
		// d update
		m_d = m_alu_r1;
		alu_update_st();
		m_icount--;
		break;

	case 0x08:
		// p update
		m_p = m_alu_r1;
		m_icount--;
		break;

	case 0x09: case 0x0a: case 0x0d:
		// d, p update
		m_d = m_alu_r1;
		m_p = m_alu_r2;
		alu_update_st();
		m_icount--;
		break;

	default:
		break;
	}
}

u16 mb86233_device::ea_pre_0(u32 r)
{
	switch(r & 0x180) {
	case 0x000: return r & 0x7f;
	case 0x080: case 0x100: return (r & 0x7f) + m_b0 + m_x0;
	case 0x180: {
		switch(r & 0x60) {
		case 0x00: return m_b0 + m_x0;
		case 0x20: return m_x0;
		case 0x40: return m_b0 + (m_x0 & m_vsmr);
		case 0x60: return m_x0 & m_vsmr;
		}
	}
	}
	return 0;
}

void mb86233_device::ea_post_0(u32 r)
{
	if(!(r & 0x100))
		return;
	if(!(r & 0x080))
		m_x0 += m_i0;
	else
		m_x0 += util::sext(r, 5);
}

u16 mb86233_device::ea_pre_1(u32 r)
{
	switch(r & 0x180) {
	case 0x000: return r & 0x7f;
	case 0x080: case 0x100: return (r & 0x7f) + m_b1 + m_x1;
	case 0x180: {
		switch(r & 0x60) {
		case 0x00: return m_b1 + m_x1;
		case 0x20: return m_x1;
		case 0x40: return m_b1 + (m_x1 & m_vsmr);
		case 0x60: return m_x1 & m_vsmr;
		}
	}
	}
	return 0;
}

void mb86233_device::ea_post_1(u32 r)
{
	if(!(r & 0x100))
		return;
	if(!(r & 0x080))
		m_x1 += m_i1;
	else
		m_x1 += util::sext(r, 5);
}

u32 mb86233_device::read_reg(u32 r)
{
	r &= 0x3f;
	if(r >= 0x20 && r < 0x30)
		return m_bus->rf_r(r & 0x1f);
	switch(r) {
	case 0x00: return m_b0;
	case 0x01: return m_b1;
	case 0x02: return m_x0;
	case 0x03: return m_x1;

	case 0x0c: return m_c0;
	case 0x0d: return m_c1;

	case 0x10: return m_a;
	case 0x11: return get_exp(m_a);
	case 0x12: return get_mant(m_a);
	case 0x13: return m_b;
	case 0x14: return get_exp(m_b);
	case 0x15: return get_mant(m_b);
	case 0x19: return m_d;
		/* c */
	case 0x1a: return get_exp(m_d);
	case 0x1b: return get_mant(m_d);
	case 0x1c: return m_p;
	case 0x1d: return get_exp(m_p);
	case 0x1e: return get_mant(m_p);
	case 0x1f: return m_sft;

	case 0x34: return m_rpc;

	default:
		mb_log("unimplemented read_reg(%02x) (%x)\n", r, m_ppc);
		return 0;
	}
}

void mb86233_device::write_reg(u32 r, u32 v)
{
	r &= 0x3f;
	if(r >= 0x20 && r < 0x30) {
		m_bus->rf_w(r & 0x1f, v);
		return;
	}
	switch(r) {
	case 0x00: m_b0 = v; break;
	case 0x01: m_b1 = v; break;
	case 0x02: m_x0 = v; break;
	case 0x03: m_x1 = v; break;

	case 0x05: m_i0 = v; break;
	case 0x06: m_i1 = v; break;

	case 0x08: m_sp = v; break;

	case 0x0a: m_vsm = v & 7; m_vsmr = (8 << m_vsm) - 1; break;

	case 0x0c:
		m_c0 = v;
		if(m_c0 == 1)
			m_st |= F_ZC0;
		else
			m_st &= ~F_ZC0;
		break;

	case 0x0d:
		m_c1 = v;
		if(m_c1 == 1)
			m_st |= F_ZC1;
		else
			m_st &= ~F_ZC1;
		break;

	case 0x0f: break;

	case 0x10: m_a = v; break;
	case 0x11: m_a = set_exp(m_a, v); break;
	case 0x12: m_a = set_mant(m_a, v); break;
	case 0x13: m_b = v; break;
	case 0x14: m_b = set_exp(m_b, v); break;
	case 0x15: m_b = set_mant(m_b, v); break;
		/* c */
	case 0x19: m_d = v; break;
	case 0x1a: m_d = set_exp(m_d, v); break;
	case 0x1b: m_d = set_mant(m_d, v); break;
	case 0x1c: m_p = v; break;
	case 0x1d: m_p = set_exp(m_p, v); break;
	case 0x1e: m_p = set_mant(m_p, v); break;
	case 0x1f: m_sft = v; break;

	case 0x34: m_rpc = v; break;
	case 0x3c: m_mask = v; break;

	default:
		mb_log("unimplemented write_reg(%02x, %08x) (%x)\n", r, v, m_ppc);
		break;
	}
}

inline void mb86233_device::write_mem_internal_1(u32 r, u32 v, bool bank)
{
	u16 ea = ea_pre_1(r);
	if(bank)
		ea += 0x200;
	if (m_fast_ram0) {
		if (ea < 0x100) m_fast_ram0[ea] = v;
		else if (ea >= 0x200 && ea < 0x400) m_fast_ram1[ea - 0x200] = v;
		else if (ea == 0x400) m_bus->data_w(0x400, v);
	} else {
		m_bus->data_w(ea, v);
	}
	ea_post_1(r);
}

inline void mb86233_device::write_mem_io_1(u32 r, u32 v)
{
	u16 ea = ea_pre_1(r);
	m_bus->io_w(ea, v);
	ea_post_1(r);
}

namespace {
enum TgpOpKind : u8 {
	OP_NOP = 0,
	OP_LAB_01_NOALU, OP_LAB_01_ALU,
	OP_LAB_3_NOALU,  OP_LAB_3_ALU,
	OP_LAB_4_NOALU,  OP_LAB_4_ALU,
	OP_MOV_01_NOALU, OP_MOV_01_ALU,
	OP_MOV_2_NOALU,  OP_MOV_2_ALU,
	OP_MOV_3_NOALU,  OP_MOV_3_ALU,
	OP_MOV_4_NOALU,  OP_MOV_4_ALU,
	OP_MOV_5_NOALU,  OP_MOV_5_ALU,
	OP_MOV_7_0_NOALU, OP_MOV_7_0_ALU,
	OP_MOV_7_1_NOALU, OP_MOV_7_1_ALU,
	OP_MOV_7_2_NOALU, OP_MOV_7_2_ALU,
	OP_MOV_7_3_NOALU, OP_MOV_7_3_ALU,
	OP_MOV_7_4_NOALU, OP_MOV_7_4_ALU,
	OP_MOV_7_5_NOALU, OP_MOV_7_5_ALU,
	OP_MOV_7_6_NOALU, OP_MOV_7_6_ALU,
	OP_STMH,
	OP_LIPL, OP_LIA, OP_LIB, OP_LID,
	OP_CLR0_NOALU, OP_CLR0_ALU,
	OP_REP_NOALU,  OP_REP_ALU,
	OP_NOP_ALU,
	OP_LDI,
	OP_BR_ALW_0,
	OP_BR_ALW_2,
	OP_BR_ALW_5,
	OP_BR
};
} // namespace

void mb86233_device::decode_prog()
{
	const u32 count = (m_prog_mask & 0xfffu) + 1u;
	for (u32 pc = 0; pc < count; ++pc) {
		const u32 opcode = m_fast_prog ? m_fast_prog[pc] : m_bus->prog_r(pc);
		DecodedOp d{};
		const u32 maj = (opcode >> 26) & 0x3f;
		switch (maj) {
		case 0x00: { // lab
			d.r1 = opcode & 0x1ff;
			d.r2 = (opcode >> 9) & 0x1ff;
			d.alu = (opcode >> 21) & 0x1f;
			const u32 op = (opcode >> 18) & 0x7;
			if (op == 0 || op == 1) d.kind = d.alu ? OP_LAB_01_ALU : OP_LAB_01_NOALU;
			else if (op == 3) d.kind = d.alu ? OP_LAB_3_ALU : OP_LAB_3_NOALU;
			else if (op == 4) d.kind = d.alu ? OP_LAB_4_ALU : OP_LAB_4_NOALU;
			else d.kind = d.alu ? OP_NOP_ALU : OP_NOP;
			break;
		}
		case 0x07: { // ld / mov
			d.r1 = opcode & 0x1ff;
			d.r2 = (opcode >> 9) & 0x1ff;
			d.alu = (opcode >> 21) & 0x1f;
			const u32 op = (opcode >> 18) & 0x7;
			switch (op) {
			case 0: case 1: d.kind = d.alu ? OP_MOV_01_ALU : OP_MOV_01_NOALU; break;
			case 2: d.kind = d.alu ? OP_MOV_2_ALU : OP_MOV_2_NOALU; break;
			case 3: d.kind = d.alu ? OP_MOV_3_ALU : OP_MOV_3_NOALU; break;
			case 4: d.kind = d.alu ? OP_MOV_4_ALU : OP_MOV_4_NOALU; break;
			case 5: d.kind = d.alu ? OP_MOV_5_ALU : OP_MOV_5_NOALU; break;
			case 7:
				switch (d.r2 >> 6) {
				case 0: d.kind = d.alu ? OP_MOV_7_0_ALU : OP_MOV_7_0_NOALU; break;
				case 1: d.kind = d.alu ? OP_MOV_7_1_ALU : OP_MOV_7_1_NOALU; break;
				case 2: d.kind = d.alu ? OP_MOV_7_2_ALU : OP_MOV_7_2_NOALU; break;
				case 3: d.kind = d.alu ? OP_MOV_7_3_ALU : OP_MOV_7_3_NOALU; break;
				case 4: d.kind = d.alu ? OP_MOV_7_4_ALU : OP_MOV_7_4_NOALU; break;
				case 5: d.kind = d.alu ? OP_MOV_7_5_ALU : OP_MOV_7_5_NOALU; break;
				case 6: d.kind = d.alu ? OP_MOV_7_6_ALU : OP_MOV_7_6_NOALU; break;
				default: d.kind = d.alu ? OP_NOP_ALU : OP_NOP; break;
				}
				break;
			default: d.kind = d.alu ? OP_NOP_ALU : OP_NOP; break;
			}
			break;
		}
		case 0x0d: {
			const u32 sub2 = (opcode >> 17) & 7;
			d.kind = (sub2 == 5) ? OP_STMH : OP_NOP;
			d.r1 = u16(opcode);
			break;
		}
		case 0x0e: {
			const u32 sub = (opcode >> 24) & 3;
			if (sub == 0) { d.kind = OP_LIPL; d.imm = opcode & 0xffffff; }
			else if (sub == 1) { d.kind = OP_LIA; d.imm = util::sext(opcode, 24); }
			else if (sub == 2) { d.kind = OP_LIB; d.imm = util::sext(opcode, 24); }
			else { d.kind = OP_LID; d.imm = util::sext(opcode, 24); }
			break;
		}
		case 0x0f: {
			d.alu = (opcode >> 20) & 0x1f;
			const u32 sub2 = (opcode >> 17) & 7;
			d.r1 = u16(opcode);
			if (sub2 == 0) d.kind = d.alu ? OP_CLR0_ALU : OP_CLR0_NOALU;
			else if (sub2 == 2) d.kind = d.alu ? OP_REP_ALU : OP_REP_NOALU;
			else d.kind = d.alu ? OP_NOP_ALU : OP_NOP;
			break;
		}
		case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f: {
			d.kind = OP_LDI;
			d.r1 = (opcode >> 24) & 0x3f;
			d.imm = util::sext(opcode, 24);
			break;
		}
		case 0x2f: case 0x3f: {
			const u32 cond = (opcode >> 20) & 0x1f;
			const u32 subtype = (opcode >> 17) & 7;
			const bool invert = (opcode & 0x40000000) != 0;
			d.r1 = u16(opcode & 0xffff);
			d.r2 = u16((opcode >> 9) & 0x3f);
			d.alu = u8(cond);
			d.extra = u16(subtype | (invert ? 8 : 0));
			if (cond == 0x16 && !invert) {
				if (subtype == 0) { d.kind = OP_BR_ALW_0; break; }
				if (subtype == 2) { d.kind = OP_BR_ALW_2; break; }
				if (subtype == 5) { d.kind = OP_BR_ALW_5; break; }
			}
			d.kind = OP_BR;
			break;
		}
		default:
			d.kind = OP_NOP;
			break;
		}
		m_decoded[pc] = d;
	}
	m_prog_dirty = false;
}

void mb86233_device::execute_run()
{
	if (__builtin_expect(m_prog_dirty, 0))
		decode_prog();

	const u32 *const fast_prog = m_fast_prog;
	const u32 prog_mask = m_prog_mask;
	u32 *const fast_ram0 = m_fast_ram0;
	u32 *const fast_ram1 = m_fast_ram1;
	const DecodedOp *const decoded = m_decoded;

	auto f_data_r = [&](u32 a) -> u32 {
		if (fast_ram0) {
			a &= 0xffff;
			if (a < 0x100) return fast_ram0[a];
			if (a >= 0x200 && a < 0x400) return fast_ram1[a - 0x200];
			if (a == 0x100) return m_bus->data_r(0x100);
			return 0;
		}
		return m_bus->data_r(a);
	};
	auto f_data_r_ram1 = [&](u32 a) -> u32 {
		if (fast_ram1) return fast_ram1[a & 0x1ff];
		return m_bus->data_r(a + 0x200);
	};

	while (m_icount > 0) {
		m_ppc = m_pc;
		const DecodedOp &d = decoded[m_pc++ & prog_mask];

#if defined(__GNUC__) || defined(__clang__)
		static const void *const kDispatch[] = {
			&&L_OP_NOP,
			&&L_OP_LAB_01_NOALU, &&L_OP_LAB_01_ALU,
			&&L_OP_LAB_3_NOALU,  &&L_OP_LAB_3_ALU,
			&&L_OP_LAB_4_NOALU,  &&L_OP_LAB_4_ALU,
			&&L_OP_MOV_01_NOALU, &&L_OP_MOV_01_ALU,
			&&L_OP_MOV_2_NOALU,  &&L_OP_MOV_2_ALU,
			&&L_OP_MOV_3_NOALU,  &&L_OP_MOV_3_ALU,
			&&L_OP_MOV_4_NOALU,  &&L_OP_MOV_4_ALU,
			&&L_OP_MOV_5_NOALU,  &&L_OP_MOV_5_ALU,
			&&L_OP_MOV_7_0_NOALU, &&L_OP_MOV_7_0_ALU,
			&&L_OP_MOV_7_1_NOALU, &&L_OP_MOV_7_1_ALU,
			&&L_OP_MOV_7_2_NOALU, &&L_OP_MOV_7_2_ALU,
			&&L_OP_MOV_7_3_NOALU, &&L_OP_MOV_7_3_ALU,
			&&L_OP_MOV_7_4_NOALU, &&L_OP_MOV_7_4_ALU,
			&&L_OP_MOV_7_5_NOALU, &&L_OP_MOV_7_5_ALU,
			&&L_OP_MOV_7_6_NOALU, &&L_OP_MOV_7_6_ALU,
			&&L_OP_STMH,
			&&L_OP_LIPL, &&L_OP_LIA, &&L_OP_LIB, &&L_OP_LID,
			&&L_OP_CLR0_NOALU, &&L_OP_CLR0_ALU,
			&&L_OP_REP_NOALU,  &&L_OP_REP_ALU,
			&&L_OP_NOP_ALU,
			&&L_OP_LDI,
			&&L_OP_BR_ALW_0,
			&&L_OP_BR_ALW_2,
			&&L_OP_BR_ALW_5,
			&&L_OP_BR
		};
		goto *kDispatch[d.kind];
#define TGP_CASE(name) L_##name:
#define TGP_END goto L_OP_DONE
#else
#define TGP_CASE(name) case name:
#define TGP_END break
		switch (d.kind) {
#endif
		TGP_CASE(OP_NOP)
			TGP_END;
		TGP_CASE(OP_LAB_01_NOALU) {
			const u32 ea1 = ea_pre_0(d.r1);
			const u32 v1 = f_data_r(ea1);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			const u32 ea2 = ea_pre_1(d.r2);
			const u32 v2 = m_bus->io_r(ea2);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			ea_post_1(d.r2);
			m_a = v1;
			m_b = v2;
			TGP_END;
		}
		TGP_CASE(OP_LAB_01_ALU) {
			alu_pre(d.alu);
			const u32 ea1 = ea_pre_0(d.r1);
			const u32 v1 = f_data_r(ea1);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			const u32 ea2 = ea_pre_1(d.r2);
			const u32 v2 = m_bus->io_r(ea2);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			ea_post_1(d.r2);
			m_a = v1;
			m_b = v2;
			alu_post_1(d.alu);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_LAB_3_NOALU) {
			const u32 ea1 = ea_pre_0(d.r1);
			const u32 v1 = f_data_r(ea1);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			const u32 ea2 = ea_pre_1(d.r2);
			const u32 v2 = f_data_r_ram1(ea2);
			ea_post_0(d.r1);
			ea_post_1(d.r2);
			m_a = v1;
			m_b = v2;
			TGP_END;
		}
		TGP_CASE(OP_LAB_3_ALU) {
			alu_pre(d.alu);
			const u32 ea1 = ea_pre_0(d.r1);
			const u32 v1 = f_data_r(ea1);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			const u32 ea2 = ea_pre_1(d.r2);
			const u32 v2 = f_data_r_ram1(ea2);
			ea_post_0(d.r1);
			ea_post_1(d.r2);
			m_a = v1;
			m_b = v2;
			alu_post_1(d.alu);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_LAB_4_NOALU) {
			const u32 ea1 = ea_pre_0(d.r1);
			const u32 v1 = f_data_r_ram1(ea1);
			const u32 ea2 = ea_pre_1(d.r2);
			const u32 v2 = f_data_r(ea2);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			ea_post_1(d.r2);
			m_a = v1;
			m_b = v2;
			TGP_END;
		}
		TGP_CASE(OP_LAB_4_ALU) {
			alu_pre(d.alu);
			const u32 ea1 = ea_pre_0(d.r1);
			const u32 v1 = f_data_r_ram1(ea1);
			const u32 ea2 = ea_pre_1(d.r2);
			const u32 v2 = f_data_r(ea2);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			ea_post_1(d.r2);
			m_a = v1;
			m_b = v2;
			alu_post_1(d.alu);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_01_NOALU) {
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = f_data_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			write_mem_io_1(d.r2, v);
			TGP_END;
		}
		TGP_CASE(OP_MOV_01_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = f_data_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			alu_post_1(d.alu);
			write_mem_io_1(d.r2, v);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_2_NOALU) {
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = m_bus->io_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			write_mem_internal_1(d.r2, v, false);
			TGP_END;
		}
		TGP_CASE(OP_MOV_2_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = m_bus->io_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			alu_post_1(d.alu);
			write_mem_internal_1(d.r2, v, false);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_3_NOALU) {
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = f_data_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			write_mem_internal_1(d.r2, v, true);
			TGP_END;
		}
		TGP_CASE(OP_MOV_3_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = f_data_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_0(d.r1);
			alu_post_1(d.alu);
			write_mem_internal_1(d.r2, v, true);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_4_NOALU) {
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = f_data_r_ram1(ea);
			ea_post_0(d.r1);
			write_mem_internal_1(d.r2, v, false);
			TGP_END;
		}
		TGP_CASE(OP_MOV_4_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = f_data_r_ram1(ea);
			ea_post_0(d.r1);
			alu_post_1(d.alu);
			write_mem_internal_1(d.r2, v, false);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_5_NOALU) {
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = fast_prog ? fast_prog[ea & prog_mask] : m_bus->prog_r(ea);
			ea_post_0(d.r1);
			write_mem_internal_1(d.r2, v, false);
			TGP_END;
		}
		TGP_CASE(OP_MOV_5_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = fast_prog ? fast_prog[ea & prog_mask] : m_bus->prog_r(ea);
			ea_post_0(d.r1);
			alu_post_1(d.alu);
			write_mem_internal_1(d.r2, v, false);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_0_NOALU) {
			const u32 v = read_reg(d.r2);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			write_mem_internal_1(d.r1, v, false);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_0_ALU) {
			alu_pre(d.alu);
			const u32 v = read_reg(d.r2);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			alu_post_1(d.alu);
			write_mem_internal_1(d.r1, v, false);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_1_NOALU) {
			const u32 v = read_reg(d.r2);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			write_mem_io_1(d.r1, v);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_1_ALU) {
			alu_pre(d.alu);
			const u32 v = read_reg(d.r2);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			alu_post_1(d.alu);
			write_mem_io_1(d.r1, v);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_2_NOALU) {
			const u32 ea = ea_pre_1(d.r1);
			const u32 v = f_data_r_ram1(ea);
			ea_post_1(d.r1);
			write_reg(d.r2, v);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_2_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_1(d.r1);
			const u32 v = f_data_r_ram1(ea);
			ea_post_1(d.r1);
			alu_post_1(d.alu);
			write_reg(d.r2, v);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_3_NOALU) {
			const u32 ea = ea_pre_1(d.r1);
			const u32 v = f_data_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_1(d.r1);
			write_reg(d.r2, v);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_3_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_1(d.r1);
			const u32 v = f_data_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_1(d.r1);
			alu_post_1(d.alu);
			write_reg(d.r2, v);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_4_NOALU) {
			const u32 ea = ea_pre_1(d.r1);
			const u32 v = m_bus->io_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_1(d.r1);
			write_reg(d.r2, v);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_4_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_1(d.r1);
			const u32 v = m_bus->io_r(ea);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			ea_post_1(d.r1);
			alu_post_1(d.alu);
			write_reg(d.r2, v);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_5_NOALU) {
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = fast_prog ? fast_prog[ea & prog_mask] : m_bus->prog_r(ea);
			ea_post_0(d.r1);
			write_reg(d.r2, v);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_5_ALU) {
			alu_pre(d.alu);
			const u32 ea = ea_pre_0(d.r1);
			const u32 v = fast_prog ? fast_prog[ea & prog_mask] : m_bus->prog_r(ea);
			ea_post_0(d.r1);
			alu_post_1(d.alu);
			write_reg(d.r2, v);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_6_NOALU) {
			const u32 v = read_reg(d.r1);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			write_reg(d.r2, v);
			TGP_END;
		}
		TGP_CASE(OP_MOV_7_6_ALU) {
			alu_pre(d.alu);
			const u32 v = read_reg(d.r1);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			alu_post_1(d.alu);
			write_reg(d.r2, v);
			alu_post_2(d.alu);
			TGP_END;
		}
		TGP_CASE(OP_STMH)
			m_m = d.r1;
			TGP_END;
		TGP_CASE(OP_LIPL)
			m_p = (m_p & 0xffffff000000ULL) | d.imm;
			TGP_END;
		TGP_CASE(OP_LIA)
			m_a = d.imm;
			TGP_END;
		TGP_CASE(OP_LIB)
			m_b = d.imm;
			TGP_END;
		TGP_CASE(OP_LID)
			m_d = d.imm;
			TGP_END;
		TGP_CASE(OP_CLR0_NOALU)
			if (d.r1 & 0x0004) m_a = 0;
			if (d.r1 & 0x0008) m_b = 0;
			if (d.r1 & 0x0010) m_d = 0;
			TGP_END;
		TGP_CASE(OP_CLR0_ALU)
			alu_pre(d.alu);
			if (d.r1 & 0x0004) m_a = 0;
			if (d.r1 & 0x0008) m_b = 0;
			if (d.r1 & 0x0010) m_d = 0;
			alu_post_1(d.alu);
			TGP_END;
		TGP_CASE(OP_REP_NOALU) {
			const u8 r = (d.r1 & 0x8000) ? u8(read_reg(d.r1)) : u8(d.r1);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			m_r = r;
			goto rep_start;
		}
		TGP_CASE(OP_REP_ALU) {
			alu_pre(d.alu);
			const u8 r = (d.r1 & 0x8000) ? u8(read_reg(d.r1)) : u8(d.r1);
			if (__builtin_expect(m_stall, 0)) goto do_stall;
			m_r = r;
			goto rep_start;
		}
		TGP_CASE(OP_NOP_ALU)
			alu_pre(d.alu);
			alu_post_1(d.alu);
			TGP_END;
		TGP_CASE(OP_LDI)
			write_reg(d.r1, d.imm);
			TGP_END;
		TGP_CASE(OP_BR_ALW_0)
			m_pc = d.r1;
			TGP_END;
		TGP_CASE(OP_BR_ALW_2)
			pcs_push();
			m_pc = d.r1;
			TGP_END;
		TGP_CASE(OP_BR_ALW_5)
			pcs_pop();
			TGP_END;
		TGP_CASE(OP_BR) {
			const u32 cond = d.alu;
			const u32 subtype = d.extra & 7;
			const bool invert = (d.extra & 8) != 0;
			bool cond_passed = false;
			switch (cond) {
			case 0x00: cond_passed = (m_st & F_ZRD) != 0; break;
			case 0x01: cond_passed = !(m_st & F_SGD); break;
			case 0x02: cond_passed = (m_st & (F_ZRD | F_SGD)) != 0; break;
			case 0x0a: cond_passed = m_gpio0; break;
			case 0x0b: cond_passed = m_gpio1; break;
			case 0x0c: cond_passed = m_gpio2; break;
			case 0x10: cond_passed = !(m_st & F_ZC0); break;
			case 0x11: cond_passed = !(m_st & F_ZC1); break;
			case 0x12: cond_passed = m_gpio3; break;
			case 0x16: cond_passed = true; break;
			default: break;
			}
			if (invert) cond_passed = !cond_passed;
			if (cond_passed) {
				switch (subtype) {
				case 0: m_pc = d.r1; break;
				case 1:
					if (d.r1 & 0x4000) {
						const u32 v = read_reg(d.r1);
						if (__builtin_expect(m_stall, 0)) goto do_stall;
						m_pc = v;
					} else {
						const u32 ea = ea_pre_0(d.r1);
						const u32 v = f_data_r(ea);
						if (__builtin_expect(m_stall, 0)) goto do_stall;
						ea_post_0(d.r1);
						m_pc = v;
					}
					break;
				case 2:
					pcs_push();
					m_pc = d.r1;
					break;
				case 3:
					if (d.r1 & 0x4000) {
						const u32 v = read_reg(d.r1);
						if (__builtin_expect(m_stall, 0)) goto do_stall;
						pcs_push();
						m_pc = v;
					} else {
						const u32 ea = ea_pre_0(d.r1);
						const u32 v = f_data_r(ea);
						if (__builtin_expect(m_stall, 0)) goto do_stall;
						ea_post_0(d.r1);
						pcs_push();
						m_pc = v;
					}
					break;
				case 5:
					pcs_pop();
					break;
				case 6: {
					const u32 ea = ea_pre_0(d.r1);
					const u32 v = f_data_r(ea);
					if (__builtin_expect(m_stall, 0)) goto do_stall;
					ea_post_0(d.r1);
					write_reg(d.r2, v);
					break;
				}
				default: break;
				}
			}
			if (subtype < 2) {
				if (cond == 0x10 && m_c0 != 1) {
					if (--m_c0 == 1) m_st |= F_ZC0;
				} else if (cond == 0x11 && m_c1 != 1) {
					if (--m_c1 == 1) m_st |= F_ZC1;
				}
			}
			TGP_END;
		}
#if !defined(__GNUC__) && !defined(__clang__)
		default:
			break;
		}
#endif
	L_OP_DONE:
#undef TGP_CASE
#undef TGP_END

		if (__builtin_expect(m_r != 1, 0)) {
			m_pc = m_ppc;
			m_r--;
		}

	rep_start:
		if (0) {
		do_stall:
			m_pc = m_ppc;
			m_stall = false;
		}
		m_icount--;
	}
}
