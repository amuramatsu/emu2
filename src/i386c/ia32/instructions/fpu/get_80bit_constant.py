#! /usr/bin/env python3

import gmpy2

def convert_data(name, f):
	INTVAL_MAX = 0x1_0000_0000_0000_0000
	INTVAL_MIN = 0x8000_0000_0000_0000
	
	sign = f < 0.0
	exp = 16383 + 63
	f = abs(f)
	if f == 0:
		exp = 0
		mantissa = 0
	else:
		if f > INTVAL_MIN:
			while f > INTVAL_MAX:
				f = f / 2
				exp += 1
		else:
			while f < INTVAL_MIN:
				f = f * 2
				exp -= 1
		mantissa = int(gmpy2.rint_round(f))
	if exp < 0 or exp > 0x8000:
		raise Exception()
	if sign:
		exp |= 0x8000
	print(f"static const uint64_t {name}_MANTISSA = {hex(mantissa)}ULL;")
	print(f"static const uint16_t {name}_EXPONENT = {hex(exp)}U;")

ctx = gmpy2.get_context()
ctx.precision = 100

VALUE_0 = gmpy2.mpfr(0.0)
VALUE_1 = gmpy2.mpfr(1.0)
VALUE_PI = gmpy2.const_pi()
VALUE_L2T = gmpy2.log2(10.0) / gmpy2.log2(2.0)
VALUE_L2E = gmpy2.log2(gmpy2.expm1(1)+1)
VALUE_LN2 = gmpy2.const_log2()
VALUE_LG2 = gmpy2.mpfr(1.0) / gmpy2.log2(10.0)

convert_data('VALUE_0', VALUE_0)
convert_data('VALUE_1', VALUE_1)
convert_data('VALUE_PI', VALUE_PI)
convert_data('VALUE_L2T', VALUE_L2T)
convert_data('VALUE_L2E', VALUE_L2E)
convert_data('VALUE_LN2', VALUE_LN2)
convert_data('VALUE_LG2', VALUE_LG2)


