/*
 *
 *	This program is a mediocre but practical stack-based floating
 *	point calculator.  It resembles the UNIX 'dc' command in usage,
 *	but is not as full-featured (no variables or arrays) and is not
 *	infinite precision -- all arithmetic is done as "double".
 *	Commands resembling some of the commands common to HP scientific
 *	calculators have been added as well.
 *		- Paul G. Fox, Wed Dec 29 1993
 *
 *
 *    The calculator has progressed since then:  at the least, I
 *    wouldn't call it mediocre anymore.  I've used it regularly (and
 *    improved it sporadically) over the decades.  It's like a good
 *    pocketknife:  always nearby, with handy features that do what I
 *    need.  Plus, if something's missing, it's usually easy to add.
 *    See the help and the man page for what it can do.  As for the
 *    name?  I needed a new, short name, and "rca" seems unused these
 *    days.  Could do worse for an RPN calculator, but given its age,
 *    and my programming style, maybe the 'r' should stand for "retro".
 *             - pgf, January 2026
 *
 *
 *  If you don't have the Makefile, build with:
 *    doit:         gcc -g -o rca -D USE_READLINE rca.c -lm -lreadline
 *    doit-norl:    gcc -g -o rca rca.c -lm
 *
 */

char *licensetext[] = {

" RCA License ",
" ---------- ",
" Copyright (C) 1993-2026  Paul Fox ",
" ",
" Redistribution and use in source and binary forms, with or without ",
" modification, are permitted provided that the following conditions ",
" are met: ",
" 1. Redistributions of source code must retain the above copyright ",
"    notice, this list of conditions and the following disclaimer. ",
" 2. Redistributions in binary form must reproduce the above copyright ",
"    notice in the documentation and/or other materials provided with ",
"    the distribution. ",
" ",
" This software is provided by the author ``as is'' and any ",
" express or implied warranties, including, but not limited to, the ",
" implied warranties of merchantability and fitness for a particular ",
" purpose, are disclaimed.  In no event shall the author be liable ",
" for any direct, indirect, incidental, special, exemplary, or ",
" consequential damages (including, but not limited to, procurement ",
" of substitute goods or services; loss of use, data, or profits; or ",
" business interruption) however caused and on any theory of liability, ",
" whether in contract, strict liability, or tort (including negligence ",
" or otherwise) arising in any way out of the use of this software, even ",
" if advised of the possibility of such damage. ",

  0 };


#ifndef VERSION
#define VERSION "v?"
#endif

#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <errno.h>
#include <locale.h>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

/* these libraries reportedly support %' for adding commas to %d, %f, etc */
#if defined(__GLIBC__) || defined(__APPLE__)
# define PRINTF_SEPARATORS 1
#else
# define PRINTF_SEPARATORS 0
#endif

/* these are filled in from the locale, if possible, otherwise
 * they'll default to period, comma, and dollar-sign */
char *decimal_pt;   // locale decimal_point
int decimal_pt_len;
char *thousands_sep, *thousands_sep_input;   // locale thousands_sep
char *currency ;   // locale currency_symbol

/* who are we? */
char *progname;

void
usage(void)
{
	fprintf(stderr, "usage: %s [ commands ]\n", progname);
	fprintf(stderr, "  'commands' will be used as initial program input\n");
	fprintf(stderr, "  Use \"%s help\" for documentation.\n", progname);
	exit(1);
}

/* debugging support, runtime controllable */
int tracing;
#define trace(a)  do {if (tracing > 1) printf a ;} while(0)

typedef int boolean;

#define TRUE 1
#define FALSE 0

typedef int opreturn;

#define GOODOP 1
#define BADOP 0

typedef long double ldouble;

/* global copies of main's argc/argv */
int g_argc;
char **g_argv;

ldouble pi = 3.141592653589793238462643383279502884L;
ldouble e =  2.718281828459045235360287471352662497L;

/* internal representation of operands on the stack.  numbers are always
 * stored as long doubles, even when we're in integer mode.  this could
 * be revisited, but since the FP mantissa is usually as big as the integer
 * word size these days (64 bits), it's probably fine.
 */
struct num {
	ldouble val;
	struct num *next;
};

/* the operand stack */
struct num *stack;
int stack_count;

/* for command repeat, like "sum" */
int stack_mark;

/* for catching infix bugs */
int infix_stacklevel;


/* all user input is either a number or a command operator.
 * this is how operators are looked up, by name
 */
typedef struct token token;
typedef struct oper oper;
typedef opreturn(*opfunc) (void);

struct oper {
	char *name;
	opfunc func;
	char *help;
	int operands;	/* used only by infix code */
	int prec;	/* used only by infix code */
};

/* operator table */
struct oper opers[];

/* tokens are typed -- currently numbers, operators, symbolic, and line-ends */
struct token {
	union {
		ldouble val;
		oper *oper;
		char *str;
	} val;
	int type;
	int alloced;
	struct token *next;  /* for stacking tokens when infix processing */
};

/* values for token type */
#define UNKNOWN -1
#define NUMERIC 0
#define SYMBOLIC 1
#define OP 2
#define EOL 3
#define LVALUE 4

/* values for # of operands field in opers table */
//  0 denotes a pseudo-op, i.e. it manipulates the calculator itself
//  1 & 2 are used verbatim as operand counts
#define Sym	-1	// a named number, like pi, or r1
#define Lval	-2	// can be assigned, like s1



/* 7 major modes:  float, decimal, unsigned, hex, octal, binary, raw
 * float.  all but float and raw float are integer modes.  "raw float"
 * is really just a debug mode:  it uses the printf %a format.
 */
int mode = 'F';			/* 'F', 'D', 'U', 'H', 'O', 'B', 'R' */
boolean floating_mode(int m) { return (m == 'F' || m == 'R'); }

/* if true, exit(4) on error, warning, or access to empty operand stack */
boolean exit_on_error = FALSE;

/* if true, print the top of stack after any line that ends with an operator */
boolean autoprint = TRUE;

/* to temporarily suppress autoprint, e.g., right after printing */
boolean suppress_autoprint = FALSE;

/* informative feedback is only printed if the command generating it
 * is followed by a newline */
void pending_printf(const char *fmt, ...);

/* if true, will decorate numbers, like "1,333,444".  */
boolean digitseparators = PRINTF_SEPARATORS;

/* Floating point precision.  This may become either the total
 * displayed precision, or the number of digits after the decimal,
 * depending on float_specifier
 */
int float_digits = 6;
int max_precision = 18;  // true value calculated at startup
int float_specifier = 'g';	/* 'f' or 'g' */
char *format_string;

/* is there a pre-defined name for this? */
#define LONGLONG_BITS (sizeof(long long) * 8)

/* These all help limit the word size to anything we want.
 */
int max_int_width;
int int_width;
long long int_sign_bit;
long long int_mask;
long long int_max;
long long int_min;

/* this is used to control floating point error */
long double epsilon;

/* allow parsing of floating hex format (e.g.  -0x8.0p-63).  enabled
 * after first use of floating hex output (with "r" or "R") */
boolean raw_hex_input_ok;

/* perform snapping and rounding of float values */
boolean do_rounding = 1;

/* the most recent top-of-stack */
ldouble lastx;

/* for store/recall */
ldouble offstack[5];

/* where input is coming from currently */
static char *input_ptr = NULL;

int parse_tok(char *p, token *t, char **nextp, boolean parsing_rpn);

void
memory_failure(void)
{
	perror("rca: malloc failure");
	exit(3);
}

void
error(const char *fmt, ...)
{
	fflush(stdout);

	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (exit_on_error)
		exit(4);
}

long long
sign_extend(ldouble a)
{
	long long b = a;

	if (int_width == LONGLONG_BITS)
		return b;
	else
		return b | (0 - (b & int_sign_bit));
}

void
toggle_warn(int n)
{
	if (n == 0 || n == 1)
		return;
	error(" warning: toggle commands usually take 0 or 1 as their argument\n");
}

opreturn
enable_errexit(void)
{
	ldouble want_errexit;
	boolean pop(ldouble *);

	if (!pop(&want_errexit))
		return BADOP;

	exit_on_error = (want_errexit != 0);

	toggle_warn(want_errexit);

	pending_printf(" errors and warnings will %s cause exit\n",
		exit_on_error ? "now" : "not");
	return GOODOP;
}

void
detect_epsilon(void)
{
	epsilon = 1.0L;

	while ((1.0L + epsilon / 2.0L) > 1.0L)
		epsilon /= 2.0L;

	// round up to significant digit
	max_precision = (int)(-log10l(epsilon)); // 18 for 64-bit mantissa
}


/* try and take care of small floating point detritus, by snapping
 * numbers that are very close to integers and zero, and by rounding
 * to our max precision.
 */
ldouble
tweak_float(ldouble x)
{

	ldouble r, abs_x, tolerance, factor;

	if (!do_rounding)
		return x;

	if (x == 0.0L || x == -0.0)
		return x;

	if (!isfinite(x))
		return x;	/* NaN, +Inf, -Inf pass through */

	abs_x = fabsl(x);

	/* snap to integer */
	/* scale tolerance by magnitude.  20 * epsilon is about 2e-18 */
	tolerance = epsilon * 20L;
	if (abs_x > 1.0L)
		tolerance *= abs_x;

	r = roundl(x);
	if (fabsl(x - r) <= tolerance) {
		if (x != r)
			trace(("snap %La (%.20Lg)\n"
				"   to %La (%.20Lg)\n", x, x, r, r));
		return r;
	}

	/* round to max_precision digits */
	factor = powl(10L, max_precision - ceill(log10l(fabsl(x))));
	r = roundl(x * factor) / factor;
	if (x != r)
		trace(("round %La (%.20Lg)\n"
			"   to %La (%.20Lg)\n", x, x, r, r));

	return r;

}

void
push(ldouble n)
{
	struct num *p;

	p = (struct num *)calloc(1, sizeof(struct num));
	if (!p)
		memory_failure();

	if (floating_mode(mode) || !isfinite(n)) {
		p->val = n;
		trace((" pushed %Lg\n", n));
	} else {
		p->val = sign_extend((long long)n & int_mask);
		trace((" pushed masked/extended %lld/0x%llx\n",
		(long long)(p->val), (long long)(p->val)));
	}

	p->next = stack;
	stack = p;
	stack_count++;
}

void
result_push(ldouble n)
{
	if (isfinite(n))
		n = tweak_float(n);
	push(n);
}

boolean
peek(ldouble *f)
{
	if (!stack)
		return FALSE;

	*f = stack->val;
	return TRUE;
}

boolean
pop(ldouble *f)
{
	struct num *p;

	p = stack;
	if (!p) {
		error(" empty stack\n");
		return FALSE;
	}
	*f = p->val;
	stack = p->next;
	trace((" popped  %Lg\n", p->val)); //, (long long)(p->val)));
	free(p);
	stack_count--;

	if (stack_count < infix_stacklevel) {
		error("BUG: stack level dropped by %d during infix\n",
				infix_stacklevel - stack_count);
	}

	/* remove a stack mark if we've gone below it */
	if (stack_count < stack_mark)
		stack_mark = 0;

	return TRUE;
}

opreturn
assignment(void)
{
	// printf("assignment called\n");
	return GOODOP;
}

opreturn
add(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			result_push(a + b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
subtract(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			result_push(a - b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
multiply(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			result_push(a * b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
divide(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			result_push(a / b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
modulo(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			result_push(fmodl(a,b));
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
y_to_the_x(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			result_push(powl(a, b));
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
e_to_the_x(void)
{
	ldouble a;

	if (pop(&a)) {
		result_push(expl(a));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

int
bothfinite(ldouble a, ldouble b)
{
	if (isfinite(a) && isfinite(b))
		return 1;

	// nan is more insidious than inf, so propagate it if present
	if (isnan(a)) {
		push(a);
		return 0;
	} else if (isnan(b)) {
		push(b);
		return 0;
	}
	if (!isfinite(a)) {
		push(a);
		return 0;
	} else if (!isfinite(b)) {
		push(b);
		return 0;
	}
	return 1;
}

opreturn
bitwise_operands_too_big(ldouble a, ldouble b)
{
	if (a < LLONG_MIN || a > LLONG_MAX ||
	    b < LLONG_MIN || b > LLONG_MAX) {
		push(a);
		push(b);
		error(" error: bitwise operand(s) bigger/smaller than LLONG_MAX/MIN\n");
		return 1;
	}
	return 0;
}

opreturn
bitwise_operand_too_big(ldouble a)
{
	if (a < LLONG_MIN || a > LLONG_MAX) {
		push(a);
		error(" error: bitwise operand bigger/smaller than LLONG_MAX/MIN\n");
		return 1;
	}
	return 0;
}

opreturn
rshift(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			unsigned long long i;  // want logical shift, not arithmetic
			long long j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (unsigned long long)a;
			j = (long long)b;
			if (j < 0) {
				error(" error: shift by negative not allowed\n");
				push(a);
				push(b);
				return BADOP;
			} else if (b >= sizeof(a) * CHAR_BIT) {
				push(0);
			} else {
				push(i >>= j);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
lshift(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			if (j < 0) {
				error(" error: shift by negative not allowed\n");
				push(a);
				push(b);
				return BADOP;
			} else if (b >= sizeof(a) * CHAR_BIT) {
				push(0);
			} else {
				push(i << j);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
bitwise_and(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			push(i & j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
bitwise_or(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			push(i | j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
bitwise_xor(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			push(i ^ j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
setbit(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			if (b < 0) {
				error(" error: negative bit number not allowed\n");
				push(a);
				push(b);
				return BADOP;
			}
			if (b >= sizeof(i) * CHAR_BIT)
				push(i);
			else
				push(i | (1LL << j));
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
clearbit(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			if (b < 0) {
				error(" error: negative bit number not allowed\n");
				push(a);
				push(b);
				return BADOP;
			}
			if (b >= sizeof(i) * CHAR_BIT)
				push(i);
			else
				push(i & ~(1LL << j));
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
bitwise_not(void)
{
	ldouble a;

	if (pop(&a)) {
		if (!isfinite(a)) {
			push(a);
			return GOODOP;
		}

		if (bitwise_operand_too_big(a))
			return BADOP;

		push(~(long long)a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
chsign(void)
{
	ldouble a;

	if (pop(&a)) {
		push(-a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
nop(void)
{
	return GOODOP;
}

opreturn
absolute(void)
{
	ldouble a;

	if (pop(&a)) {
		push((a < 0) ? -a : a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
recip(void)
{
	ldouble a;

	if (pop(&a)) {
		result_push(1.0 / a);
		return GOODOP;
	}
	return BADOP;
}

opreturn
squarert(void)
{
	ldouble a;

	if (pop(&a)) {
		result_push(sqrtl(a));
		return GOODOP;
	}
	return BADOP;
}

opreturn
trig_no_sense(void)
{
	error(" error: trig functions make no sense in integer mode");
	return BADOP;
}

int trig_degrees = 1;  // work in degrees by default

opreturn
use_degrees(void)
{
	ldouble wantdegrees;

	if (!pop(&wantdegrees))
		return BADOP;

	toggle_warn(wantdegrees);

	trig_degrees = (wantdegrees != 0);

	pending_printf(" trig functions will now use %s\n",
		trig_degrees ? "degrees" : "radians");
	return GOODOP;
}

ldouble
to_degrees(ldouble angle)
{
	return (angle * 180.0L) / pi;
}

ldouble
to_radians(ldouble angle)
{
	return (angle * pi) / 180.0L;
}

ldouble
radians_to_user_angle(ldouble rads)
{
	if (trig_degrees)
		return to_degrees(rads);
	else
		return rads;
}

ldouble
user_angle_to_radians(ldouble u_angle)
{
	if (trig_degrees)
		return to_radians(u_angle);
	else
		return u_angle;
}

ldouble
user_angle_to_degrees(ldouble u_angle)
{
	if (trig_degrees)
		return u_angle;
	else
		return to_degrees(u_angle);
}

opreturn
sine(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(sinl(user_angle_to_radians(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
asine(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(radians_to_user_angle(asinl(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
cosine(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(cosl(user_angle_to_radians(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
acosine(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(radians_to_user_angle(acosl(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
tangent(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		// tan() goes undefined at +/-90
		if (fmodl(tweak_float(user_angle_to_degrees(a)) - 90, 180) == 0)
			result_push(NAN);
		else
			result_push(tanl(user_angle_to_radians(a)));
		lastx = a;
		return GOODOP;
	}

	return BADOP;
}

opreturn
atangent(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(radians_to_user_angle(atanl(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
atangent2(void)
{
	ldouble a,b;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&b)) {
		if (pop(&a)) {
			result_push(radians_to_user_angle(atan2l(a,b)));
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
log_worker(int which)
{
	ldouble n, l;

	if (pop(&n)) {
		switch(which) {
		default:  // warning suppression
		case 0: l = logl(n); break;
		case 2: l = log2l(n); break;
		case 10:l = log10l(n); break;
		}
		result_push(l);
		lastx = n;
		return GOODOP;
	}
	return BADOP;
}

opreturn
log_natural(void)
{
	return log_worker(0);
}

opreturn
log_base2(void)
{
	return log_worker(2);
}

opreturn
log_base10(void)
{
	return log_worker(10);
}

opreturn
fraction(void)
{
	ldouble a;

	if (pop(&a)) {
		if (a > 0)
			result_push(a - floorl(a));
		else
			result_push(a - ceill(a));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
integer(void)
{
	ldouble a;

	if (pop(&a)) {
		if (a > 0)
			result_push(floorl(a));
		else
			result_push(ceill(a));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
logical_and(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a && b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
logical_or(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a || b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_eq(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a == b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_neq(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a != b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_lt(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a < b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_le(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a <= b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_gt(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a > b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_ge(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a >= b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
logical_not(void)
{
	ldouble a;

	if (pop(&a)) {
		push((a == 0) ? 1 : 0);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
clear(void)
{
	ldouble scrap;

	if (peek(&lastx))
		while(stack) pop(&scrap);

	return GOODOP;
}

opreturn
rolldown(void)			// aka "pop"
{
	(void)pop(&lastx);
	return GOODOP;
}

opreturn
enter(void)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		push(a);
		return GOODOP;
	}
	return BADOP;
}

// during an infix evaluation, lastx needs to kept at its pre-infix value.
boolean lastx_is_frozen = 0;
ldouble frozen_lastx;

void
freeze_lastx(void)
{
	if (!lastx_is_frozen) {
		if (!peek(&frozen_lastx))
			frozen_lastx = 0;
		lastx_is_frozen = TRUE;
		infix_stacklevel = stack_count;
	}
}

void
thaw_lastx(void)
{
	if (lastx_is_frozen) {
		lastx_is_frozen = FALSE;
		lastx = frozen_lastx;
		if (stack_count != infix_stacklevel + 1)
			error("BUG: stack changed by %d after infix\n",
				stack_count - infix_stacklevel);
		infix_stacklevel = -1;
	}
}

opreturn
repush(void)			// aka "lastx"
{
	if (lastx_is_frozen)
		push(frozen_lastx);
	else
		push(lastx);
	return GOODOP;
}

opreturn
exchange(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(b);
			push(a);
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}


char pending_info[1024];

void
pending_clear(void)
{
	*pending_info = '\0';
}

void
pending_flush(void)
{
	if (*pending_info) {
		printf("%s", pending_info);
		*pending_info = '\0';
	}
}

void
pending_printf(const char *fmt, ...)
{
	int n, remaining;
	va_list ap;
	static int used;

	if (!pending_info[0]) used = 0;

	remaining = sizeof(pending_info) - used;
	if (remaining <= 0) return;

	va_start(ap, fmt);
	n = vsnprintf(pending_info + used, remaining, fmt, ap);
	va_end(ap);

	if (n >= 0 && n < remaining)
		used += n;
	else
		used += remaining;

}

void
putbinarybyte(long long mask, unsigned long long n)
{
	int i;

	n &= 0xff;
	mask &= 0xff;

	for (i = 0x80; i != 0; i >>= 1) {
		if (mask & i)
			putchar(n & i ? '1' : '0');
	}
}

void
putbinary2(int width, long long mask, unsigned long long n)
{
	int i, bytes = (width + 7) / 8;

	for (i = bytes - 1; i >= 0; i--) {
		putbinarybyte(mask >> (8 * i), n >> (8 * i));
		if (digitseparators && i >= 1)	// commas every 8 bits
			fputs(thousands_sep, stdout);
	}
}

void
putbinary(long long n)
{
	// mode can be float but we can print in binary
	if (floating_mode(mode))	// no masking in float mode
		putbinary2(max_int_width, ~0, (unsigned long long)n);
	else
		putbinary2(int_width, int_mask, (unsigned long long)n);
}

void
puthex(unsigned long long n)
{
	/* commas every 4 hex digits */
	if (n < 0x10000) {
		printf("%llx", n);
		return;
	}
	puthex((n / 0x10000));
	if (digitseparators)
		fputs(thousands_sep, stdout);
	printf("%04llx", n % 0x10000);
}

void
putoct(unsigned long long n)
{
	/* commas every 3 octal digits */
	if (n < 01000) {
		printf("%llo", n);
		return;
	}
	putoct(n / 01000);
	if (digitseparators)
		fputs(thousands_sep, stdout);
	printf("%03llo", n % 01000);
}

boolean
check_int_truncation(ldouble *np, boolean conv)
{
	ldouble n = *np;
	boolean changed = 0;

	if (isnan(n) || !isfinite(n)) {
		n = sign_extend(int_sign_bit);
		changed = 1;
	} else if (n != sign_extend((long long)n & int_mask)) {
		n = sign_extend((long long)n & int_mask);
		changed = 1;
	}

	if (conv)
		*np = n;

	return changed;
}

void
show_int_truncation(boolean changed, ldouble old_n)
{
	if (!changed) {
		putchar('\n');
		return;
	}

	if (floating_mode(mode))
		error("     # warning: display format loses accuracy\n");
	else
		error("     # warning: accuracy lost, was %.*Lg\n", max_precision, old_n);

}

int
match_dp(char *p)
{
	return (strncmp(p, decimal_pt, decimal_pt_len) == 0);
}

int
min(int a, int b)
{
	return (a < b) ? a : b;
}

void
print_floating(ldouble n, int format)
{
	putchar(' ');
	if (format == 'R') {

		raw_hex_input_ok = TRUE;

		/* NB:  this printing format is accurate and exact,
		 * but may vary from machine to machine because printf
		 * may canonicalize the mantissa differently depending
		 * on hardware or who knows what.  There can be at
		 * least 4 different combinations of first digit and
		 * exponent that all represent the same number.  */
		// 1 digit per 4 bits, and 1 of them is before the decimal
		printf("%.*La\n", (LDBL_MANT_DIG + 3)/4 - 1, n);

	} else if (format == 'F' && float_specifier == 'f') {
		char buf[128];
		char *p;
		int decimals, leadingdigits = 0;

		/* The goal is to reduce the number of non-significant
		 * digits shown.  If decimals is set to 6, then 123e12
		 * would show "123,000,000,000,000.000000" which is
		 * showing more than max_precision significant digits.  As
		 * long as some of the excess digits are beyond the
		 * decimal point, we can trim some from that end.  So the
		 * code below gives us "123,000,000,000,000.000".  But that
		 * only works for a while.  If the number gets 1e6 times
		 * bigger, then we have this: "123,000,000,000,000,000,000",
		 * and we're back to showing more than we should.  (At that
		 * point the user should be switching to %g mode.)
		 */
		snprintf(buf, sizeof(buf), format_string, float_digits, n);

		for (p = buf; *p && !match_dp(p); p++) {
			if (isdigit(*p))
				leadingdigits++;
		}

		// in "0.34", the 0 doesn't count toward significant digits
		if (leadingdigits == 1 && *buf == '0')
			leadingdigits = 0;

		if (p) { /* found a decimal point */

			decimals = min(float_digits, max_precision - leadingdigits);
			if (decimals <= 0) decimals = 0;

			snprintf(buf, sizeof(buf), format_string, decimals, n);
		}
		puts(buf);

	} else {
		printf(format_string, float_digits, n);
		putchar('\n');
	}

}


void
print_n(ldouble *np, int format, boolean conv)
{
	ldouble old_n, n;
	long long ln;
	long long mask = int_mask;
	unsigned long long uln;
	boolean changed;

	suppress_autoprint = TRUE;

	old_n = n = *np;

	if (floating_mode(format) || !isfinite(n)) {
		print_floating(n, format);
		return;
	}

	/* this is a little messy.  this is the general "print a
	 * number" routine, but because it's called at the deep end of
	 * a recursive loop when printing a stack, it also gets
	 * saddled for converting values on the stack.  the conversion
	 * needs to happen when switching from float mode to an
	 * integer mode:  values need to be masked and sign extended
	 * (if word length is less than native), and we need to do it
	 * here so we can print a message about the conversion
	 * alongside the converted value.
	 */


	/* check for integer masking, and optionally modify the original.
	 * we'll do that if we're changing modes, but not if we're just
	 * printing in another mode's format.
	 */
	changed = check_int_truncation(&n, conv);

	/* mode can be float but we can still print in hex, binary
	 * format, etc
	 */
	switch (format) {
	case 'H':
		ln = (long long)n & mask;
		printf(" 0x");
		puthex(ln);
		break;
	case 'O':
		ln = (long long)n & mask;
		printf(" 0");
		putoct(ln);
		break;
	case 'B':
		ln = (long long)n & mask;
		printf(" 0b");
		putbinary(ln);
		break;
	case 'U':
		// convert in two steps, to avoid possibly undefined
		// negative double to unsigned conversion
		ln = (long long)n & mask;
		uln = (unsigned long long)ln;
		printf(digitseparators ? " %'llu" : " %llu", uln);
		break;
	case 'D':
		ln = (long long)n;
		if (floating_mode(mode) || int_width == LONGLONG_BITS) {
			printf(digitseparators ? " %'lld" : " %lld", ln);
		} else {
			/* shenanigans to make pos/neg numbers appear
			 * properly.  our masked/shortened numbers
			 * don't appear as negative to printf, so we
			 * find the reduced-width sign bit, and fake
			 * it.
			 */
			long long t;

			mask = (long long)int_mask & ~int_sign_bit;
			if (ln & int_sign_bit) {	// negative
				t = int_sign_bit - (ln & mask);
				printf(" -");
			} else {
				t = ln & mask;
				printf(" ");
			}
			printf(digitseparators ? "%'lld" : "%lld", t);
		}
		break;
	default:
		error(" bug: default case in print_n()\n");
		return;
	}

	show_int_truncation(changed, old_n);
	if (changed)
		*np = n;

}

void
print_top(int format)
{
	if (stack)
		print_n(&stack->val, format, 0);
}

void
printstack(boolean conv, struct num *s)
{
	if (!s)
		return;

	if (s->next)
		printstack(conv, s->next);

	print_n(&(s->val), mode, conv);
}

opreturn
printall(void)
{
	printstack(0,stack);
	return GOODOP;
}

opreturn
printone(void)
{
	print_top(mode);
	return GOODOP;
}

opreturn
printhex(void)
{
	print_top('H');
	return GOODOP;
}

opreturn
printoct(void)
{
	print_top('O');
	return GOODOP;
}

opreturn
printuns(void)
{
	print_top('U');
	return GOODOP;
}

opreturn
printrawhex(void)
{
	print_top('R');
	return GOODOP;
}

opreturn
printbin(void)
{
	print_top('B');
	return GOODOP;
}

opreturn
printdec(void)
{
	print_top('D');
	return GOODOP;
}

opreturn
printfloat(void)
{
	print_top('F');
	return GOODOP;
}

opreturn
printstate(void)
{
	struct num *s;

	putchar('\n');
	printf(" Current mode is %c\n", mode);
	putchar('\n');
	printf(" In floating mode:\n");
	printf("  max precision is %u decimal digits\n", max_precision);
	printf("  current display mode is \"%d %s\"\n",
		float_digits, float_specifier == 'f' ? "decimals" : "precision");
	printf("  format string is \"%s\"\n", format_string);
	printf("  snapping/rounding is %s\n", do_rounding ? "on" : "off");
	putchar('\n');

	printf(" In integer modes:\n");
	printf("  width is %d bits\n", int_width);
	printf("  mask:     0x"); puthex(int_mask);	putchar('\n');
	printf("  sign bit: 0x"); puthex(int_sign_bit);	putchar('\n');
	printf("  max:      0x"); puthex(int_max);	putchar('\n');
	printf("  min:      0x"); puthex(int_min);	putchar('\n');
	putchar('\n');

	s = stack;
	printf(" Stack, top comes first:\n");
	if (!s) {
		printf("%16s\n", "<empty>");
	} else {
		printf(" %20s   %20s\n",
		    "long long", "long double ('%#20.20Lg' and '%La')");
		while (s) {
			printf(" %#20llx   %#20.20Lg    %La\n",
				(long long)(s->val), s->val, s->val);
			s = s->next;
		}
	}
	printf(" stack count %d, stack mark %d\n", stack_count, stack_mark);

	putchar('\n');
	printf("\n Build-time sizes:\n");
	printf("  Native sizes (bits):\n");
	printf("   sizeof(long long):\t%lu\n", (unsigned long)(8 * sizeof(long long)));
	printf("   LLONG_MIN: %llx, LLONG_MAX: %llx\n", LLONG_MIN, LLONG_MAX);
	printf("   sizeof(long double):\t%lu\n", (unsigned long)(8 * sizeof(long double)));
	printf("   LDBL_MANT_DIG: %u\n", LDBL_MANT_DIG);
	printf("   LDBL_MAX: %.20Lg\n", LDBL_MAX);
	printf("   LDBL_EPSILON is %Lg (%La)\n", LDBL_EPSILON, LDBL_EPSILON);
	printf("  Calculated:\n");
	printf("   detected epsilon is %Lg (%La)\n", epsilon, epsilon);
	putchar('\n');
	printf(" Locale elements (%s):\n", setlocale(LC_NUMERIC, NULL));
	printf("  decimal '%s', thousands separator '%s', currency '%s'\n",
		decimal_pt ?: "null", thousands_sep ?: "null", currency ?: "null");

	suppress_autoprint = TRUE;
	return GOODOP;
}

static char *
mode2name(void)
{
	switch (mode) {
	case 'D':
		return "signed decimal";
	case 'U':
		return "unsigned decimal";
	case 'O':
		return "octal";
	case 'H':
		return "hex";
	case 'B':
		return "binary";
	case 'R':
		return "raw hex float";
	case 'F':
	default: // can't happen.  set it to default
		mode = 'F';
		return "float";
	}
}

void
showmode(void)
{

	pending_printf(" Mode is %s. ", mode2name());

	if (mode == 'F') {
		char *msg;

		if (float_specifier == 'g') {
			/* float_digits == 7 gives:  123.4560  */
			msg = "of total precision";
		} else {	/* 'f' */
			/* float_digits == 7 gives:  123.4560000  */
			msg = "after the decimal";
		}
		pending_printf(" Displaying %u digits %s.\n", float_digits, msg);
	} else if (mode == 'R') {
		pending_printf(" Displaying using floating hexadecimal.\n");
	} else {
		pending_printf(" Integer math with %d bits.\n", int_width);
	}

	suppress_autoprint = TRUE;
}

opreturn
modeinfo(void)
{
	showmode();
	return GOODOP;
}

opreturn
modehex(void)
{
	mode = 'H';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
moderawhex(void)
{
	mode = 'R';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modebin(void)
{
	mode = 'B';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modeoct(void)
{
	mode = 'O';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modedec(void)
{
	mode = 'D';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modeuns(void)
{
	mode = 'U';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modefloat(void)
{
	mode = 'F';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

void
setup_format_string(void)
{
	/* The floating print options include
	   - commas or not
	   - %f or %g
	   - we used to use alternate form ('%#') with %f, but not %g, but I
		don't recall why.  so we don't anymore.  this lets '0 K'
		cause an integral value to print without decimal point.
	   Also:
	   - precision
	   but happily that's provided via the '*' specifier at printf time.

	   So there are just four forms to deal with here.
	 */

	if (digitseparators) {
		if (float_specifier == 'f')
			format_string = "%'.*Lf";
		else
			format_string = "%'.*Lg";
	} else {
		if (float_specifier == 'f')
			format_string = "%.*Lf";
		else
			format_string = "%.*Lg";
	}
}

opreturn
separators(void)
{
	ldouble wantsep;

	if (!pop(&wantsep))
		return BADOP;

	toggle_warn(wantsep);

	if (!thousands_sep[0]) {
		pending_printf(" No thousands separator defined in the "
			"current locale. so no numeric separators.\n");
		digitseparators = 0;
		return GOODOP;
	}

	digitseparators = (wantsep != 0);

	setup_format_string();

	pending_printf(" Numeric separators now %s\n",
		digitseparators ? "on" : "off");

	return GOODOP;
}

opreturn
precision(void)
{
	ldouble digits;
	char *limited = "";

	if (!pop(&digits))
		return BADOP;

	float_digits = abs((int)digits);
	// this is total digits, so '0' doesn't make sense
	if (float_digits < 1) {
		float_digits = 1;
	} else if (float_digits > max_precision) {
		float_digits = max_precision;
		limited = "the maximum of ";
	}

	float_specifier = 'g';

	setup_format_string();

	pending_printf(" Will show %s%d significant digit%s.\n", limited,
		float_digits, float_digits == 1 ? "" : "s");

	if (mode != 'F')
		pending_printf(" Not in floating decimal mode, float precision"
				" recorded but ignored.\n");

	return GOODOP;
}

opreturn
decimal_length(void)
{
	ldouble digits;

	if (!pop(&digits))
		return BADOP;

	// this is digits after decimal, so '0' is okay
	float_digits = abs((int)digits);

	// but it can't be greater than our maximum precision
	if (float_digits > max_precision)
		float_digits = max_precision;

	float_specifier = 'f';

	setup_format_string();

	if (float_digits == 0)
		pending_printf(" Will show no digits after the decimal.\n");
	else
		pending_printf(" Will show at most %d digit%s after the decimal.\n",
			float_digits, float_digits == 1 ? "" : "s");

	if (mode != 'F')
		pending_printf(" Not in floating decimal mode, decimal"
				" length is recorded but ignored.\n");


	return GOODOP;
}

void
setup_width(int bits)
{
	/* we use long double to store our data.  in integer mode,
	 * this means the FP mantissa, if it's shorter than long long,
	 * may limit our maximum word width.
	 */
	if (!bits || !max_int_width) {	/* first call */
		max_int_width = LONGLONG_BITS;
		if (max_int_width > LDBL_MANT_DIG)
			max_int_width = LDBL_MANT_DIG;
		bits = max_int_width;
	}

	if (bits > max_int_width)
		bits = max_int_width;

	int_width = bits;
	int_sign_bit = (1LL << (int_width - 1));

	if (int_width == LONGLONG_BITS) {
		int_mask = ~0;
		int_max = LLONG_MAX;
		int_min = LLONG_MIN;
	} else {
		int_mask = (1LL << int_width) - 1;
		int_max = int_mask >> 1;
		int_min = int_sign_bit;
	}
}

void
mask_stack(void)
{
	struct num *s;
	for (s = stack; s; s = s->next) {
		if (isfinite(s->val))
			s->val = sign_extend((long long)s->val & int_mask);
	}
}

opreturn
width(void)
{
	ldouble n;
	long long bits;

	if (!pop(&n))
		return BADOP;

	bits = n;
	if (bits == 0) {
		bits = max_int_width;
	} else if (bits > max_int_width) {
		bits = max_int_width;
		printf(" Width out of range, set to max (%lld)\n", bits);
	} else if (bits < 2) {
		bits = 2;
		printf(" Width out of range, set to min (%lld)\n", bits);
	}

	setup_width(bits);

	pending_printf(" Integers are now %d bits wide.\n", int_width);
	if (floating_mode(mode)) {
		pending_printf(" In floating mode, integer width"
				" is recorded but ignored.\n");
	} else {
		mask_stack();
	}

	return GOODOP;
}

opreturn
store_any(int loc)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		offstack[loc - 1] = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
recall_any(int loc)
{
	push(offstack[loc - 1]);
	return GOODOP;
}

opreturn
store1(void)
{
	return store_any(1);
}

opreturn
store2(void)
{
	return store_any(2);
}

opreturn
store3(void)
{
	return store_any(3);
}

opreturn
store4(void)
{
	return store_any(4);
}

opreturn
store5(void)
{
	return store_any(5);
}

opreturn
recall1(void)
{
	return recall_any(1);
}

opreturn
recall2(void)
{
	return recall_any(2);
}

opreturn
recall3(void)
{
	return recall_any(3);
}

opreturn
recall4(void)
{
	return recall_any(4);
}

opreturn
recall5(void)
{
	return recall_any(5);
}

opreturn
push_pi(void)
{
	result_push(pi);
	return GOODOP;
}

opreturn
push_e(void)
{
	result_push(e);
	return GOODOP;
}

opreturn
mark(void)
{
	ldouble n;
	if (!pop(&n))
		return BADOP;

	if (n > stack_count || n < -1) {
		if (stack_count == 0)
			error(" error: bad mark, max of 0 with empty stack, or, -1 to clear\n");
		else
			error(" error: bad mark, range between 0 and stack length (%d), or -1 to clear\n", stack_count);
	}

	if (n == -1)
		stack_mark = 0; // special case:  clear the mark
	else
		stack_mark = stack_count - n;
	return GOODOP;
}

opreturn
sum_worker(boolean do_sum)
{
	opreturn r;
	ldouble a, tot = 0, n = 0;

	if (stack_count <= stack_mark) {
		error(" error: nothing to %s\n", do_sum ? "sum":"avg");
		return BADOP;
	}

	while (stack_count > stack_mark) {
		if ((r = pop(&a)) == BADOP)
			break;
		tot += a;
		n++;
	}

	stack_mark = 0;

	if (do_sum)
		result_push(tot);
	else
		result_push(tot/n);

	return r;
}

opreturn
sum(void)
{
	return sum_worker(1);
}

opreturn
avg(void)
{
	return sum_worker(0);
}

opreturn
units_in_mm(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 25.4;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_mm_in(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 25.4;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_ft_m(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 3.28084;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_m_ft(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 3.28084;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_F_C(void)
{
	ldouble a;

	if (pop(&a)) {
		a -= 32.0;
		a /= 1.8;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_C_F(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 1.8;
		a += 32.0;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_l_qt(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 1.05669;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_qt_l(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 1.05669;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_oz_g(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 28.3495;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_g_oz(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 28.3495;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_oz_ml(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 29.5735;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_ml_oz(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 29.5735;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_mi_km(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 0.6213712;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_km_mi(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 0.6213712;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_deg_rad(void)
{
	ldouble a;

	if (pop(&a)) {
		a = to_radians(a);
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_rad_deg(void)
{
	ldouble a;

	if (pop(&a)) {
		a = to_degrees(a);
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

token *out_stack, *oper_stack, *infix_rpn_queue;

char *
stackname(token **tstackp)
{
	char *n;
	if (tstackp == &oper_stack)
		n = "operator stack";
	else if (tstackp == &out_stack)
		n = "output stack";
	else if (tstackp == &infix_rpn_queue)
		n = "rpn output";
	else
		n = "unknown stack";
	return n;
}


void
tpush(token **tstackp, token *token)
{
	struct token *t;

	/* if we originally malloc'ed the incoming token, just reuse
	 * it, otherwise malloc and copy.
	 */
	if (token->alloced) {
		t = token;
	} else {
		t = (struct token *)calloc(1, sizeof(struct token));
		if (!t)
			memory_failure();

		*t = *token;
		t->alloced = 1;
	}

	t->next = *tstackp;
	*tstackp = t;
}

token *
tpeek(token **tstackp)
{
	return *tstackp;
}

token *
tpop(token **tstackp)
{
	struct token *rt;

	rt = *tstackp;
	if (!rt) {
		return NULL;
	}

	*tstackp = (*tstackp)->next;

	return rt;
}

void
tclear(token **tstackp)
{
	token *t, *nt;

	t = *tstackp;
	*tstackp = NULL;

	while (t) {
		nt = t->next;
		free(t);
		t = nt;
	}
}

void
sprint_token(char *s, int slen, token *t)
{
	switch (t->type) {
	case NUMERIC:
		snprintf(s, slen, "'%.*Lg'", max_precision, t->val.val);
		break;
	case SYMBOLIC:
	case LVALUE:
		snprintf(s, slen, "'%s'", t->val.oper->name);
		break;
	case OP:
		snprintf(s, slen, "'%s'", t->val.oper->name);
		break;
	case EOL:
		snprintf(s, slen, "'EOL'");
		break;
	case UNKNOWN:
		snprintf(s, slen, "'unknown (%d)'", t->type);
		break;
	}
}

void
show_tok(token *t)
{
	static char buf[128];

	sprint_token(buf, sizeof(buf), t);
	printf(" %s ", buf);
}

void
stack_dump(token **tstackp)
{
	token *t = *tstackp;

	printf("%s: ", stackname(tstackp));
	if (!t)
		printf("<empty>");
	else
		while (t) {
			show_tok(t);
			t = t->next;
		}
	printf("\n");
}

void
tdump(token **tstackp)
{
	if (tracing > 1)
		stack_dump(tstackp);
}

token open_paren_token, chsign_token, nop_token;

void
create_infix_support_tokens()
{
	/* we need a couple of token identifiers for later on,
	 * specifically for dealing with infix processing.
	 */
	char *outp;
	(void)parse_tok("(", &open_paren_token, &outp, 0);
	(void)parse_tok("chs", &chsign_token, &outp, 0);
	(void)parse_tok("nop", &nop_token, &outp, 0);
}

void
expression_error(token *pt, token *t)
{
	char pts[128], ts[128];
	sprint_token(pts, 128, pt);
	sprint_token(ts, 128, t);
	error(" error: bad expression sequence, at %s and %s\n", pts, ts);
}

opreturn
close_paren(void)
{
	// this has to be a warning -- the command in error is already
	// finished, so we can't cancel it.
	error(" warning: mismatched/extra parentheses\n");
	return BADOP;
}

boolean
prev_tok_was_operand(token *pt)
{
	return pt->type == NUMERIC ||
		pt->type == SYMBOLIC ||
		(pt->type == OP && pt->val.oper->func == close_paren);
}

/* This implementation of Dijkstra's shunting yard algorithm is based
 * on pseudocode from Wikipedia and brilliant.org, on several of the
 * coded examples at rosettacode.org, and on pseudo-code generated by
 * an AI bot.
 */
opreturn
open_paren(void)
{
	static struct token tok, prevtok;
	int paren_count;
	token *t, *pt, *tp;	// pointers to tok, prevtok, and tpeek'ed token

#define t_op t->val.oper	// shorthands.  don't use unless type == OP
#define tp_op tp->val.oper
#define pt_op pt->val.oper

	tclear(&out_stack);
	tclear(&oper_stack);

	// push the '(' token the user typed
	tpush(&oper_stack, &open_paren_token);
	paren_count = 1;
	prevtok = open_paren_token;
	pt = &prevtok;

	while (1) {
		trace(("\n"));
		tdump(&oper_stack);
		tdump(&out_stack);

		while (isspace(*input_ptr))
			input_ptr++;

		if (!*input_ptr)
			break;

		t = &tok;

		if (!parse_tok(input_ptr, &tok, &input_ptr, 0)) {
			goto cleanup;
		}

		if (pt->type == LVALUE &&
			!(t->type == OP && t_op->func == assignment)) {
			expression_error(pt, t);
			input_ptr = NULL;
			return BADOP;
		}

		switch (t->type) {
		case LVALUE:
			if (pt->type != OP || pt->val.oper->func != open_paren) {
				expression_error(pt, t);
				input_ptr = NULL;
				return BADOP;
			}
			tpush(&oper_stack, t);
			break;
		case NUMERIC:
		case SYMBOLIC:
			if (prev_tok_was_operand(pt)) {
				expression_error(pt, t);
				input_ptr = NULL;
				return BADOP;
			}
			tpush(&out_stack, t);
			break;
		case OP:
			tp = tpeek(&oper_stack);

			if (t_op->func == open_paren) {
				if (prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}
				// Push opening parenthesis to operator stack
				tpush(&oper_stack, t);
				paren_count++;
			} else if (t_op->func == close_paren) {
				if (!prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}
				// Process until matching opening paren
				while ((tp = tpeek(&oper_stack))) {
					if (tp == NULL) {
						error(" error: missing parentheses?\n");
						return BADOP;
					}

					if (tp_op->func == open_paren)
						break;

					tpush(&out_stack, tpop(&oper_stack));
				}

				// Pop the opening parenthesis
				free(tpop(&oper_stack));
				tp = tpeek(&oper_stack);
				if (tp && tp_op->operands == 1) {
					tpush(&out_stack, tpop(&oper_stack));
				}
				paren_count--;

			} else if (t_op->operands == 1) { // one operand, like "~"
			unary:
				if (prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}
				while (tp != NULL &&
					(tp_op->func != open_paren) &&
					(tp_op->prec > t_op->prec))
				{
					tpush(&out_stack, tpop(&oper_stack));
					tp = tpeek(&oper_stack);
				}
				tpush(&oper_stack, t);

			} else if (t_op->operands == 2) { // two operands
				/*
				 * +/- are unary if the previous token isn't
				 *   something that will become an operand,
				 *   and if they're followed by something that
				 *   isn't space or the end of expression.
				 *   Also, we don't let them be followed by
				 *   +/-, to prevent weird multiple +-+
				 *   sequences.  If that's not all true,
				 *   they're binary operators.
				 * TL;DR: +/- are unary if prev token is a
				 *   non-')' operator and next char is
				 *   whitespace, ), +, -, or \0
				 */
				if ( (t_op->func == subtract ||
				      t_op->func == add) &&
					!prev_tok_was_operand(pt) &&
					!strchr(" \t\v\r\n)+-", *input_ptr)) {
					if (t_op->func == subtract) {
						t = &chsign_token;
						trace((" subtract is now chs\n"));
					} else {  // add
						t = &nop_token;
						trace((" add is now nop\n"));
					}
					goto unary;
				}

				if (t_op->func == assignment) {
					if (pt->type != LVALUE) {
						expression_error(pt, t);
						input_ptr = NULL;
						return BADOP;
					}
					// assignment is a no-op, no token push
					break;
				}

				if (!prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}

				// Handle binary operators
				while (tp != NULL &&
					(tp_op->func != open_paren) &&
					(tp_op->prec >= t_op->prec))
				{
					/* a ** b is right-associative */
					if (tp_op->prec == t_op->prec &&
					    tp_op->func == y_to_the_x)
						break;

					tpush(&out_stack, tpop(&oper_stack));
					tp = tpeek(&oper_stack);
				}
				tpush(&oper_stack, t);
			} else {
				error(" error: '%s' unsuitable in infix expression\n",
					t_op->name);
				input_ptr = NULL;
				return BADOP;
			}
			break;

		default:
		case UNKNOWN:
		cleanup:
			error(" error: unrecognized input '%s'\n", t->val.str);
			input_ptr = NULL;
			return BADOP;
		}

		if (paren_count == 0)
			break;

		prevtok = *t;

	}
	trace(("\nfinished reading expression\n"));

	if (paren_count) {
		error(" error: missing parentheses\n");
		return BADOP;
	}

	/* the shunting yard is finished.  output stack is in the
	 * wrong order, so one more transfer to reverse it.
	 * gettoken() will pull from this copy.
	 */
	while((t = tpop(&out_stack)) != NULL) {
		tpush(&infix_rpn_queue, t);
	}

	if (tracing) {
		trace(("\nmerged and reversed:\n"));
		printf("\n");
		stack_dump(&infix_rpn_queue);
	}

	return GOODOP;

#undef t_op
#undef tp_op
}


opreturn
autop(void)
{
	ldouble wantautop;

	if (!pop(&wantautop))
		return BADOP;

	toggle_warn(wantautop);

	autoprint = (wantautop != 0);

	pending_printf(" Autoprinting is now %s\n", autoprint ? "on" : "off");
	return GOODOP;
}

/* debug support */
opreturn
tracetoggle(void)
{
	ldouble wanttracing;

	if (!pop(&wanttracing))
		return BADOP;

	if (wanttracing == 11) {
		opreturn commands(void);
		return commands();
	}

	// two levels currently, mostly for infix processing.
	// 1 is just input and rpn token logging.
	// 2 is full shunting algorithm logging, and also snapping/rounding
	tracing = wanttracing;

	printf(" internal tracing is now level %d\n", tracing);
	return GOODOP;
}

opreturn
rounding(void)
{
	ldouble wantrounding;

	if (!pop(&wantrounding))
		return BADOP;

	toggle_warn(wantrounding);

	do_rounding = (wantrounding != 0);

	pending_printf( " Float snapping/rounding is now %s\n",
		do_rounding ? "on" : "off");
	return GOODOP;
}

void
exitret(void)
{
	ldouble a = 0;
	if (stack) {  // exit 0 or 1, based on top of stack
		pop(&a);
		exit(a == 0);  // flip exit status per unix convention
	} else {
		exit(2);  // exit 2 on empty stack
	}

}

opreturn
quit(void)
{
	if (!suppress_autoprint && autoprint)
		print_top(mode);
	exitret();
	return GOODOP; // not reached
}

size_t stralnum(char *s, char **endptr)
{
	char *ns = s;
	while (isalnum(*ns) || *ns == '_')
		ns++;
	*endptr = ns;
	return ns - s;
}

int
parse_tok(char *p, token *t, char **nextp, boolean parsing_rpn)
{
	int sign = 1;

	if (parsing_rpn && (*p == '+' || *p == '-')) {
		/* In RPN, be sure + and - are bound closely to
		 * numbers.  We want "1 2 -3" to push "1", "2", and
		 * "-3", not "(1-2)", and "3".  For infix expressions,
		 * whether to bind the +/- to a number depends on what
		 * came before.  "(2-3)" should be read as "(2 - 3)",
		 * not "(2 -3)".  That's handled in open_paren(),
		 * where we can track ordering.
		 */
		if (*p == '+' && (match_dp(p + 1) || isdigit(*(p + 1)))) {
			p++;
		} else if (*p == '-' && (match_dp(p + 1) || isdigit(*(p + 1)))) {
			sign = -1;
			p++;
		} else if (isspace(*(p+1)) || *(p+1) == 0) {
			goto is_oper;
		} else {
			goto unknown;
		}
	}

	if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X')) {
		// hex
		long double dd;
		if (raw_hex_input_ok) {
			// will allow floating hex, e.g. 0xc.90fdaa22168c23cp-2
			dd = strtold(p, nextp);
		} else {
			// simple hex integers only
			dd = strtoull(p, nextp, 16);
		}
		if (dd == 0.0 && p == *nextp)
			goto unknown;

		t->val.val = dd * sign;
		t->type = NUMERIC;

	} else if (*p == '0' && (*(p + 1) == 'b' || *(p + 1) == 'B')) {
		// binary
		long long ln = strtoull(p + 2, nextp, 2);

		if (ln == 0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = ln * sign;

	} else if (*p == '0' && ('0' <= *(p + 1) && *(p + 1) <= '7')) {
		// octal
		long long ln = strtoull(p, nextp, 8);

		if (ln == 0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = ln * sign;

	} else if (isdigit(*p) || match_dp(p)) {
		// decimal
		long double dd = strtold(p, nextp);

		if (dd == 0.0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = dd * sign;
	} else {
		int n;

	    is_oper:
		if (isalpha(*p)) {
			n = stralnum(p, nextp);
		} else if (ispunct(*p)) {
			/* parser hack:  hard-coded list of
			 * double punctuation operators */
			if (    (p[0] == '>' && p[1] == '>') ||      //   >>
				(p[0] == '<' && p[1] == '<') ||      //   <<
				(p[0] == '>' && p[1] == '=') ||      //   >=
				(p[0] == '<' && p[1] == '=') ||      //   <=
				(p[0] == '=' && p[1] == '=') ||      //   ==
				(p[0] == '!' && p[1] == '=') ||      //   !=
				(p[0] == '&' && p[1] == '&') ||      //   &&
				(p[0] == '|' && p[1] == '|') ||      //   ||
				(p[0] == '*' && p[1] == '*')) {      //   **
				n = 2;
			} else {
				n = 1;
			}
			*nextp = p + n;
		} else {
			error(" error: illegal character in input\n");
			t->val.str = p;
			t->type = UNKNOWN;
			return 0;
		}

		// command
		oper *op;

		op = opers;
		while (op->name) {
			int matchlen;

			if (!op->func) {
				op++;
				continue;
			}
			matchlen = strlen(op->name);
			if (n == matchlen && !strncmp(op->name, p, matchlen)) {
				*nextp = p + matchlen;
				t->val.oper = op;
				if (op->operands == Sym) // like "pi", "recall"
					t->type = SYMBOLIC;
				else if (op->operands == Lval)
					t->type = LVALUE;
				else
					t->type = OP;
				break;
			}
			op++;
		}
		if (!op->name) {
		unknown:
			t->val.str = p;
			t->type = UNKNOWN;
			return 0;
		}
	}
	if (tracing) show_tok(t);
	return 1;
}

char *
strremoveall(char *haystack, const char *needle)
{
	size_t len = strlen(needle);

	if (len > 0) {
		char *p = haystack;

		while ((p = strstr(p, needle)) != NULL)
			memmove(p, p + len, strlen(p + len) + 1);
	}
	return haystack;
}

void
no_comments(char *cp)
{
	char *ncp;

	/* Eliminate comments */
	if ((ncp = strchr(cp, '#')) != NULL)
		*ncp = '\0';

	/* Eliminate the thousands separator from numbers, like
	 * "1,345,011".  This removes them from the entire line, would
	 * be a problem except:  the only simple ascii separators used
	 * in locales are '.' and ',', which we don't use anywhere
	 * else.  (Removing '.' is safe, because if the separator is
	 * '.', then the decimal point isn't.)  All the others are
	 * unicode sequences, which we don't use either.  So the
	 * command line won't be harmed by this removal.  Some locales
	 * use a space as a separator, but it's a "hard" space,
	 * represented as unicode.
	 */
	if (thousands_sep_input[0])
		strremoveall(cp, thousands_sep_input);

	/* Same for currency symbols.  They're mostly unicode
	 * sequences or punctuation (e.g., '$'), which are safe to
	 * remove.  But some are plain ascii.  We checked earlier to
	 * be sure the currency symbol doesn't match any of our
	 * commands.
	 */
	if (currency && currency[0])
		strremoveall(cp, currency);
}

#ifdef USE_READLINE
char *
command_generator(const char *prefix, int state)
{
	static int len;
	static struct oper *op;

	/* If this is the first time called, initialize our state. */
	if (!state) {
		op = opers - 1;
		len = strlen(prefix);
	}

	/* Return the next name in the list that matches our prefix. */
	while (op++) {
		if (!op->name)
			break;
		if (!op->name[0])
			continue;
		if (!op->func)
			continue;
		if (strncmp(op->name, prefix, len) != 0)
			continue;
		return strdup(op->name);
	}

	return 0;
}

/* Attempted completion function. */
char **
command_completion(const char* prefix, int start, int end)
{
	(void)start;  // suppress "unused" warnings
	(void)end;

	return rl_completion_matches(prefix, command_generator);
}
#endif

static int o_stdout_fd = -1;

void
suppress_stdout(void)
{
	fflush(stdout);

	o_stdout_fd = dup(STDOUT_FILENO);

	int null_fd = open("/dev/null", O_WRONLY);
	if (null_fd == -1) {	// would be surprising
		o_stdout_fd = -1;
		return;
	}

	dup2(null_fd, STDOUT_FILENO);	// Redirect stdout to /dev/null
	close(null_fd);		// Close the /dev/null file descriptor
}

void
restore_stdout(void)
{
	if (o_stdout_fd == -1)
		return;
	fflush(stdout);
	dup2(o_stdout_fd, STDOUT_FILENO);	// Restore original stdout
	close(o_stdout_fd);
	o_stdout_fd = -1;
}

/* on return, the global input_ptr is a string containing commands
 * to be executed
 */
int
fetch_line(void)
{
	static int arg = 1;
	static char *input_buf;
	static size_t blen;
	static boolean tried_rca_init;
	char *rca_init;

	if (!tried_rca_init) {
		tried_rca_init = TRUE;
		rca_init = getenv("RCA_INIT");
		if (rca_init) {
			suppress_stdout();
			blen = strlen(rca_init) + 1;
			input_buf = malloc(blen);
			if (!input_buf)
				memory_failure();
			strcpy(input_buf, rca_init);
			input_ptr = input_buf;
			return 1;
		}
	}

	restore_stdout();

	/* if there are args on the command line, they're taken as
	 * initial commands.  since only numbers can start with '-',
	 * any other use of a hyphen brings up a usage message.
	 */
	if (arg < g_argc) {

		if (g_argv[1][0] == '-' && !(isdigit(g_argv[1][1])))
			usage();

		if (input_buf) free(input_buf);

		blen = 0;
		for (arg = 1; arg < g_argc; arg++)
			blen += strlen(g_argv[arg]) + 2;

		input_buf = malloc(blen);
		if (!input_buf)
			memory_failure();

		*input_buf = '\0';
		for (arg = 1; arg < g_argc; arg++) {
			strcat(input_buf, g_argv[arg]);
			strcat(input_buf, " ");
		}

		no_comments(input_buf);

		input_ptr = input_buf;
		return 1;
	}

#ifdef USE_READLINE
	static char readline_init_done = 0;

	if (!readline_init_done) {
		rl_readline_name = "rca";
		rl_basic_word_break_characters = " \t\n";
		rl_attempted_completion_function = command_completion;
		using_history();
		readline_init_done = 1;
	}

	/* if we used the buffer as input, add it to history.  doing
	 * this here records any command line input, possibly stored
	 * in the buffer above, on the first call to fetch_line()
	 */
	if (input_buf && *input_buf)
		add_history(input_buf);

	if (input_buf) free(input_buf);

	if ((input_buf = readline("")) == NULL)  // got EOF
		exitret();

#if READLINE_NO_ECHO_BARE_NL
	/* a bug in readline() doesn't echo bare newlines to a tty if
	 * the program has no prompt.  so we do it here.  this is
	 * needed in some sub-versions of readline 8.2 */
	if (*input_buf == '\0')
		putchar('\n');
#endif

#else // no readline()

	if (getline(&input_buf, &blen, stdin) < 0)  // EOF
		exitret();

	if (input_buf[strlen(input_buf) - 1] == '\n')
		input_buf[strlen(input_buf) - 1] = '\0';

	/* if stdin is a terminal, the command is already on-screen.
	 * but we also want it mixed with the output if we're
	 * redirecting from a file or pipe.  (easy to get rid of it
	 * with something like: "rca < commands | grep '^ '"
	 */
	if (!isatty(0))
		printf("%s\n", input_buf);
#endif

	no_comments(input_buf);

	input_ptr = input_buf;

	return 1;
}

int
gettoken(struct token *t)
{
	char *next_input_ptr;
	if (input_ptr == NULL) {
		if (!fetch_line())
			return 0;
	}

	while (isspace(*input_ptr))
		input_ptr++;

	if (*input_ptr == '\0') {	/* out of input */
		t->type = EOL;
		input_ptr = NULL;
		return 1;
	}

	if (tracing)
		printf("input 'tokens':");

	fflush(stdin);

	if (!parse_tok(input_ptr, t, &next_input_ptr, 1)) {
		error(" error: unrecognized input '%s'\n", input_ptr);
		input_ptr = NULL;
		return 0;
	}

	input_ptr = next_input_ptr;
	return 1;
}


opreturn
precedence(void)
{
	oper *op;
#define NUM_PRECEDENCE 30
	static char *prec_ops[NUM_PRECEDENCE] = {0};
	int linelen[NUM_PRECEDENCE] = {0};
	int prec, i;
	int unary_prec = 0;
	int y_to_x_prec = 0;
	char *prefix;
	static int precedence_generated;

	printf(" Precedence for operators in infix expressions, from \n"
	       "  top to bottom in order of descending precedence.\n"
	       " All operators are left-associative, except for those\n"
	       "  in rows marked 'R', which associate right to left.\n");
	if (!precedence_generated) {
		prefix = "";
		op = opers;
		while (op->name) {

			/* skip anything in the table that doesn't have
			 * a name, a function, or a precedence */
			if (!op->name[0] || !op->func || op->prec == 0) {
				op++;
				continue;
			}

			if (op->prec >= NUM_PRECEDENCE) {
				error("error: %s precedence too large: %d\n",
					op->name, op->prec);
			}
			if (!prec_ops[op->prec]) {
				prec_ops[op->prec] = (char *)calloc(1, 500);
				if (!prec_ops[op->prec])
					memory_failure();
				linelen[op->prec] = 12;
			}
			if (strcmp(op->name, "chs") == 0) {
				strcat(prec_ops[op->prec], "+ - ");
				linelen[op->prec] += 4;
				unary_prec = op->prec;
			}
			if (strcmp(op->name, "^") == 0)
				y_to_x_prec = op->prec;
			strcat(prec_ops[op->prec], op->name);
			strcat(prec_ops[op->prec], " ");
			linelen[op->prec] += strlen(op->name) + 1;
			if (linelen[op->prec] > 70) {
				linelen[op->prec] = 12;
				prefix = "\n            ";
			} else {
				strcat(prec_ops[op->prec], prefix);
				prefix = "";
			}
			op++;
		}


		precedence_generated = 1;
	}

	i = 1;
	for (prec = NUM_PRECEDENCE-1; prec >=0; prec--) {
		if (prec_ops[prec]) {
			printf(" %-2i  %c     %s\n", i,
			(prec <= unary_prec && prec >= y_to_x_prec) ? 'R':' ',
			prec_ops[prec]);
			i++;
		}
	}

	return GOODOP;
}

opreturn
commands(void)
{
	oper *op, *lastop = NULL;

	op = opers;

	printf("%10s%10s %s %s\n", "oper alias", "operands", "preced", "help");
	printf("---- -----  -------- ------ -------\n");
	while (op->name) {
		if (op->func ) {
			if (lastop && lastop->func == op->func )
				printf("%10s\t%d\t%d\t%s\n",
				op->name, op->operands, op->prec,
					op->help ? op->help : "");
			else
				printf("%-10s\t%d\t%d\t%s\n",
				op->name, op->operands, op->prec,
					op->help ? op->help : "");
		}
		lastop = op;
		op++;
	}
	return GOODOP;
}

opreturn
license(void)
{
	int i = 0;

	while (licensetext[i])
		printf("%s\n", licensetext[i++]);

	return GOODOP;
}

opreturn
help(void)
{
	oper *op;

	FILE *fout;
	boolean fout_is_pipe = 0;
	char *pager = getenv("PAGER");

	if (pager && pager[0] && isatty(fileno(stdout)) &&
			(fout = popen(pager, "w"))) {
		printf("Using '%s' (from $PAGER) to show help text\n", pager);
		fout_is_pipe = 1;
	} else {
		fout = stdout;
	}



	op = opers;
	fprintf(fout, "\
 rca -- a rich/RPN scientific and programmer's calculator\n\
  Any arguments on the command line are used as initial calculator input.\n\
  Entering a number pushes it on the stack.\n\
  Operators replace either one or two stack values with their result.\n\
  Most whitespace is optional between numbers and operators.\n\
  Input can include locale currency%s symbols: %s12%s345%s67\n\
  Always prefix hex (0x7f) or octal (0177) input, even in hex or octal mode.\n\
  Infix expressions are entered using (...), as in: (sin(30)^2 + cos(30)^2)\n\
  Below, 'x' refers to top-of-stack, 'y' refers to the next value beneath.\n\
  rca's normal exit value reflects the logical value of the top of stack.\n\
\n\
",
	thousands_sep_input[0] ? " and grouping" : "",
		currency, thousands_sep_input, decimal_pt);

	char cbuf[1000];
	opfunc prevfunc;

	cbuf[0] = '\0';
	prevfunc = 0;

	while (op->name) {
		if (!*op->name) {
			fprintf(fout, "\n");
		} else {
			if (!op->func) {
				fprintf(fout, " %s\n", op->name);
			} else {
				if (cbuf[0]) { // continuing
					if (op->func == prevfunc)
						strcat(cbuf, op->help ?
							", or " : ", ");
					else
						strcat(cbuf, ", ");
				} else {
					strcat(cbuf, " ");
				}
				strcat(cbuf, op->name);
				if (op->help) {
					fprintf(fout, "%21s     %s\n",
						cbuf, op->help);
					cbuf[0] = '\0';
				}
			}
		}
		prevfunc = op->func;
		op++;
	}
	fprintf(fout, "\n%78s\n", "version " VERSION " built " __DATE__ " " __TIME__);

	if (!fout_is_pipe) {
		// tip not needed if a pager's already in use
		fprintf(fout, "\n Tip:	Use \"rca help q | less\""
				" to view this help\n");
		return GOODOP;
	}

	if (pclose(fout) != 0)
		printf(" Failed showing help. Unset PAGER to show help directly\n");
	else
		printf(" (Help ended)\n");

	return GOODOP;
}

void
locale_init(void)
{
	struct lconv *lc;

	setlocale(LC_ALL, "");

	lc = localeconv();

	/* fetch the decimal point */
	decimal_pt = lc->decimal_point;
	decimal_pt_len = strlen(decimal_pt);

	/* fetch the thousands separator
	 * thousands_sep will be used only for output
	 * thousands_sep_input only for input */
	thousands_sep_input = thousands_sep = lc->thousands_sep;

	/* if there's no thousands separator, default the one, that
	 * will clean up program input to ",", but only if the decimal
	 * point is ".".  this lets us safely strip commas from input
	 * even if the locale isn't set up */
	if (!thousands_sep_input[0]) {
		if (strcmp(decimal_pt, ".") == 0)
			thousands_sep_input = ",";
	}

	/* fetch the currency symbol.  default to '$' */
	currency = lc->currency_symbol;
	if (!currency[0]) {
		/* as above, this lets us strip '$', even if the
		 * locale isn't set up.  we don't use currency for
		 * output purposes */
		currency = "$";
	} else {
		/* make sure the currency symbol doesn't conflict with
		 * any command names, because we're going to simply
		 * delete the symbol from input lines before parsing.
		 * A few are known to match, or be substrings of, our
		 * commands.
		 */

		/* first check if it's unicode.  if so, no worries. */
		if (!isascii(*currency))
			return;

		/* then search for it in the command list */
		oper *op = opers;
		while (op->name) {
			if (strstr(op->name, currency)) {
				currency = 0;
				break;
			}
			op++;
		}
	}

}

/* the opers[] table doesn't initialize everything explicitly */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// *INDENT-OFF*.
struct oper opers[] = {
//       +-------------------------------- section header if no function ptr
//       |
//       |
//       V
    {"Numerical operators with two operands:"},
//        +------------------------------- operator names
//        |    +-------------------------- function pointer
//        |    |                +--------- help (if 0, shares next cmd's help)
//        |    |                |  +------ # of operands (0 means none (pseudop),
//        |    |                |  |        "Sym" -1 means none (named constant)
//        |    |                |  |        "Lval" -2 means none (can be assigned)
//        |    |                |  |  +--- operator precedence
//        |    |                |  |  |         (# of operands and precedence
//        V    V                V  V  V           used only by infix code)
	{"+", add,		0, 2, 18 },
	{"-", subtract,		"Add and subtract x and y", 2, 18 },
	{"*", multiply,		0, 2, 20 },
	{"x", multiply,		"Multiply x and y", 2, 20 },
	{"/", divide,		0, 2, 20 },
	{"%", modulo,		"Divide and modulo of y by x", 2, 20 },
	{"^", y_to_the_x,	0, 2, 22 },
	{"**", y_to_the_x,	"Raise y to the x'th power", 2, 22 },
	{">>", rshift,		0, 2, 16 },
	{"<<", lshift,		"Right/left logical shift of y by x bits", 2, 16 },
	{"&", bitwise_and,	0, 2, 14 },
	{"|", bitwise_or,	0, 2, 10 },
	{"xor", bitwise_xor,	"Bitwise AND, OR, and XOR of y and x", 2, 12 },
	{"setb", setbit,	0, 2, 10 },
	{"clearb", clearbit,	"Set and clear bit x in y", 2, 14 },
	{"=", assignment,	"Assignment (to storage locations", 2, 1},
	{""},		// all-null entries cause blank line in output
    {"Numerical operators with one operand:"},
	{"~", bitwise_not,	"Bitwise NOT of x (1's complement)", 1, 26 },
	{"chs", chsign,		0, 1, 26 },
	{"negate", chsign,	"Change sign of x (2's complement)", 1, 26 },
	{"nop", nop,		"Does nothing", 1, 26 },
	{"recip", recip,	0, 1, 26 },
	{"sqrt", squarert,	"Reciprocal and square root of x", 1, 26 },
	{"sin", sine,		0, 1, 26 },
	{"cos", cosine,		0, 1, 26 },
	{"tan", tangent,	"", 1, 26 },
	{"asin", asine,		0, 1, 26 },
	{"acos", acosine,	0, 1, 26 },
	{"atan", atangent,	"Trig functions", 1, 26 },
	{"atan2", atangent2,	"Arctan of y/x (2 operands)", 2, 26 },
	{"exp", e_to_the_x,	"Raise e to the x'th power", 1, 26 },
	{"ln", log_natural,	0, 1, 26 },
	{"log2", log_base2,	0, 1, 26 },
	{"log10", log_base10,	"Natural, base 2, and base 10 logarithms", 1, 26 },

	{"abs", absolute,	0, 1, 26 },
	{"frac", fraction,	0, 1, 26 },
	{"int", integer,	"Absolute value, fractional and integer parts of x", 1, 26 },
	{"(", open_paren,	0, 0, 28 },
	{")", close_paren,	"Begin and end \"infix\" expression", 0},
	{""},
    {"Logical operators (mostly two operands):"},
	{"&&", logical_and,	0, 2, 4 },
	{"||", logical_or,	"Logical AND and OR", 2, 2 },
	{"==", is_eq,		0, 2, 6 },
	{"!=", is_neq,		0, 2, 6 },
	{"<", is_lt,		0, 2, 8 },
	{"<=", is_le,		0, 2, 8 },
	{">", is_gt,		0, 2, 8 },
	{">=", is_ge,		"Arithmetic comparisons", 2, 8 },
	{"!", logical_not,	"Logical NOT of x", 1, 26 },
	{""},
    {"Stack manipulation:"},
	{"clear", clear,	"Clear stack" },
	{"pop", rolldown,	"Pop (and discard) x" },
	{"push", enter,		0 },
	{"dup", enter,		"Push (a duplicate of) x" },
	{"lastx", repush,	0, Sym },
	{"lx", repush,		"Fetch previous value of x", Sym },
	{"exch", exchange,	0 },
	{"swap", exchange,	"Exchange x and y" },
	{"mark", mark,		"Mark stack for later summing" },
	{"sum", sum,		"Sum stack to \"mark\", or entire stack if no mark" },
	{"avg", avg,		"Average stack to \"mark\", or entire stack if no mark" },
	{""},
    {"Constants and storage (no operands):"},
	{"store", store1,	0, Lval },
	{"recall", recall1,	"Same as s1 and r1", Sym },
	{"s1", store1,		0, Lval },
	{"s2", store2,		0, Lval },
	{"s3", store3,		0, Lval },
	{"s4", store4,		0, Lval },
	{"s5", store5,		"Save x off-stack (to 5 locations)", Lval },
	{"r1", recall1,		0, Sym },
	{"r2", recall2,		0, Sym },
	{"r3", recall3,		0, Sym },
	{"r4", recall4,		0, Sym },
	{"r5", recall5,		"Fetch x (from 5 locations)", Sym },
	{"pi", push_pi,		"Push constant pi", Sym },
	{"e", push_e,		"Push constant e", Sym },
	{""},
    {"Unit conversions (one operand):"},
	{"i2mm", units_in_mm,	0, 1, 26 },
	{"mm2i", units_mm_in,	"inches / millimeters", 1, 26 },
	{"ft2m", units_ft_m,	0, 1, 26},
	{"m2ft", units_m_ft,	"feet / meters", 1, 26 },
	{"mi2km", units_mi_km,	0, 1, 26 },
	{"km2mi", units_km_mi,	"miles / kilometers", 1, 26 },
	{"f2c", units_F_C,	0, 1, 26 },
	{"c2f", units_C_F,	"degrees F/C", 1, 26 },
	{"oz2g", units_oz_g,	0, 1, 26 },
	{"g2oz", units_g_oz,	"ounces / grams", 1, 26 },
	{"oz2ml", units_oz_ml,	0, 1, 26 },
	{"ml2oz", units_ml_oz,	"ounces / milliliters", 1, 26 },
	{"q2l", units_qt_l,	0, 1, 26 },
	{"l2q", units_l_qt,	"quarts / liters", 1, 26 },
	{"d2r", units_deg_rad,	0, 1, 26 },
	{"r2d", units_rad_deg,	"degrees / radians", 1, 26 },
	{""},
    {"Display:"},
	{"P", printall,		"Print whole stack according to mode" },
	{"p", printone,		"Print x according to mode" },
	{"f", printfloat,	0 },
	{"d", printdec,		0 },
	{"u", printuns,		"Print x as float, decimal, unsigned decimal," },
	{"h", printhex,		0 },
	{"o", printoct,		0 },
	{"b", printbin,		"     hex, octal, or binary" },
	{"state", printstate,	"Show calculator state" },
	{""},
    {"Modes:"},
	{"F", modefloat,	0 },
	{"D", modedec,		0 },
	{"U", modeuns,		"Switch to floating point, decimal, unsigned decimal," },
	{"H", modehex,		0 },
	{"O", modeoct,		0 },
	{"B", modebin,		"     hex, octal, or binary modes" },
	{"precision", precision, 0 },
	{"k", precision,	"Float format: number of significant digits (%g)" },
	{"decimals", decimal_length, 0 },
	{"K", decimal_length,	"Float format: digits after decimal (%f)" },
	{"width", width,	0 },
	{"w", width,		"Set effective word size for integer modes" },
	{"degrees", use_degrees, "Toggle trig functions: degrees (1) or radians (0)" },
	{"autoprint", autop,	0 },
	{"a", autop,		"Toggle autoprinting on/off with 0/1" },
#if PRINTF_SEPARATORS
	{"separators", separators, 0 },
	{"s", separators,	"Toggle numeric separators (i.e., commas) on/off (0/1)" },
#endif
	{"mode", modeinfo,	"Display current mode parameters" },
	{""},
    {"Debug support:"},
	{"r", printrawhex,	"Print x as raw floating hex" },
	{"R", moderawhex,	"Switch to raw floating hex mode"},
	{"rounding", rounding,	"Toggle snapping and rounding of floats" },
	{"tracing", tracetoggle,"Toggle debug tracing" },
//	{"commands", commands,	"Dump raw command table" }, # use "11 tracing"
	{""},
    {"Housekeeping:"},
	{"?", help,		0 },
	{"help", help,		"Show this list (using $PAGER, if set)" },
	{"precedence", precedence, "List infix operator precedence" },
	{"quit", quit,		0 },
	{"q", quit,		0 },
	{"exit", quit,		"Leave the calculator" },
	{"errorexit", enable_errexit,	"Toggle exiting on error and warning" },
	{"license", license,	"Display the rca copyright and license." },
	{"#", help,		"Comment. The rest of the line will be ignored." },
	{NULL, NULL, 0},
};
// *INDENT-ON*.

int
main(int argc, char *argv[])
{
	struct token tok;
	token *t;
	static int lasttoktype;
	char *pn;
	opreturn opret = BADOP;

	pn = strrchr(argv[0], '/');
	progname = pn ? (pn + 1) : argv[0];

	/* fetch_line() will process args as if they were input as commands */
	g_argc = argc;
	g_argv = argv;

	locale_init();

	setup_width(0);
	setup_format_string();
	detect_epsilon();

	create_infix_support_tokens();

	/* we simply loop forever, either pushing operands or
	 * executing operators.  the special end-of-line token lets us
	 * do reasonable autoprinting, if the last thing on the line
	 * was an operator.
	 */
	while (1) {

		// use up tokens created by infix processing first */
		if (tpeek(&infix_rpn_queue) && (t = tpop(&infix_rpn_queue))) {
			tok = *t;
			free(t);
			freeze_lastx();
		} else { // otherwise get tokens from input as usual
			if (!gettoken(&tok))
				continue;
			thaw_lastx();
		}
		t = &tok;

		if (t->type != EOL)
			pending_clear();

		switch (t->type) {
		case NUMERIC:
			result_push(t->val.val);
			break;
		case LVALUE:
		case SYMBOLIC:
		case OP:
			trace(( "invoking %s\n", t->val.oper->name));
			opret = (t->val.oper->func) ();
			break;
		case EOL:
                        pending_flush();
			if (!suppress_autoprint && autoprint &&
				(lasttoktype == OP || lasttoktype == SYMBOLIC) &&
				opret == GOODOP) {
				print_top(mode);
			}
			suppress_autoprint = FALSE;
			break;
		default:
		case UNKNOWN:
			error(" error: unrecognized input '%s'\n", t->val.str);
			break;
		}

		lasttoktype = t->type;

	}
	exit(3);  // not reached
}
