/*
 *	This program is a mediocre but practical stack-based floating
 *	point calculator.  It resembles the UNIX 'dc' command in usage,
 *	but is not as full-featured (no variables or arrays) and is not
 *	infinite precision -- all arithmetic is done as "double".
 *	Commands resembling some of the commands common to HP scientific
 *	calculators have been added as well.
 *		- Paul G. Fox, Wed Dec 29 1993
 *
 *	[Math is done in "long double", as of 2012.]
 *
 *	It's probably still mediocre, but it still works.  Over the years
 *	its gained some unit conversions, the ability to work in integer vs.
 *	float, the ability to display and accept hex and octal, and I added
 *	readline support, for good measure.
 *		- pgf, Tue Feb 13, 2024
 *
 *  build with:
 *	doit:      gcc -g -o ca -D USE_READLINE ca.c -lm -lreadline
 *	doit-norl: gcc -g -o ca ca.c -lm
 *  test support:
 *	doit-test:    egrep -v '^ ' ca_test.txt | ca | diff -u ca_test.txt -
 *	doit-newtest: egrep -v '^ ' ca_test.txt | ca > new_ca_test.txt
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <errno.h>
#include <locale.h>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

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

#define DEBUG 1
#undef DEBUG

#ifdef DEBUG
#define debug(a) printf a
#else
#define debug(a)
#endif

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

/* all user input is either a number or a command operator.
 * this is how operators are looked up, by name
 */
typedef struct token token;
typedef struct oper oper;

struct oper {
	char *name;
	 opreturn(*func) (token *);
	char *help;
};

/* operator table */
struct oper opers[];

/* tokens are typed -- currently numbers, operators, line-ends */
struct token {
	union {
		ldouble val;
		oper *oper;
		char *str;
	} val;
	int type;
};

/* values for token.type */
#define NUMERIC 0
#define OP 1
#define EOL 2
#define UNKNOWN -1

/* if true, print the top of stack after any line that ends with an operator */
boolean autoprint = TRUE;
boolean suppress_autoprint = FALSE;

/* if true, will decorate decimals "1,333,444".   binary bytes also get the
 * spa treatment, where commas separate bytes.
 */
boolean punct = TRUE;

/* default floating point precision */
int float_digits = 6;

// is there a pre-defined name for this?
#define LONGLONG_BITS (sizeof(long long) * 8)

/* Dealing with all 64 bits when you just want to do some 16 bit hex
 * math is a pain, so we can limit the word size to anything we want.
 * These all help do that.
 */
int max_int_width;
int int_width;
long long int_sign_bit;
long long int_mask;

/* 4 modes: float, decimal integer, hex, and octal.
 * the last 3 are integer modes.
 */
int mode = 'f';			/* or 'd', 'h', or 'o' */

/* decimal, hex, or octal output.  normally matches mode, but
 * can be changed by individual commands
 */
int print_format = 'f';		/* 'f', 'd', 'h', or 'o' */

/* used to suppress "empty stack" messages */
boolean empty_stack_ok = FALSE;

/* the most recent top-of-stack */
ldouble lastx;

/* for store/recall */
ldouble offstack;
ldouble offstack2;
ldouble offstack3;

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
push(ldouble n)
{
	struct num *p = (struct num *)calloc(1, sizeof(struct num));

	if (!p) {
		perror("calloc");
		exit(1);
	}

	if (mode != 'f') {
		p->val = sign_extend((long long)n & int_mask);
		debug(("pushed s/e 0x%Lx\n", (long long)(p->val)));
	} else {
		p->val = n;
		debug(("pushed %Lg/0x%Lx as-is\n", n, (long long)(p->val)));
	}

	p->next = stack;
	stack = p;
}

boolean
pop(ldouble *f)
{
	struct num *p;

	p = stack;
	if (!p) {
		if (!empty_stack_ok)
			printf(" empty stack\n");
		return FALSE;
	}
	*f = p->val;
	stack = p->next;
	free(p);
	return TRUE;
}

opreturn
add(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a + b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
subtract(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a - b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
multiply(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a * b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
divide(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (b != 0.0) {
				push(a / b);
			} else {
				push(a);
				push(b);
				printf(" would divide by zero\n");
				return BADOP;
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
modulo(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			i = (long long)a;
			j = (long long)b;
			if (j != 0) {
				push(i / j);
			} else {
				push(a);
				push(b);
				printf(" would divide by zero\n");
				return BADOP;
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
y_to_the_x(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (a >= 0 || floorl(b) == b) {
				push(powl(a, b));
			} else {
				push(a);
				push(b);
				printf(" result would be complex\n");
				return BADOP;
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
rshift(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			i = (long long)a;
			j = (long long)b;
			push((i >> j) & ~int_sign_bit);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
lshift(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			i = (long long)a;
			j = (long long)b;
			push(i << j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
and(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

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
or(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

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
xor(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

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
setbit(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			i = (long long)a;
			j = (long long)b;
			push(i | (1LL << j));
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
clearbit(token *t)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			i = (long long)a;
			j = (long long)b;
			push(i & ~(1LL << j));
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
not(token *t)
{
	ldouble a;

	if (pop(&a)) {
		push(~(long long)a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
chsign(token *t)
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
absolute(token *t)
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
recip(token *t)
{
	ldouble a;

	if (pop(&a)) {
		if (a != 0.0) {
			push(1.0 / a);
			lastx = a;
			return GOODOP;
		} else {
			push(a);
			printf(" would divide by zero\n");
			return BADOP;
		}
	}
	return BADOP;
}

opreturn
squarert(token *t)
{
	ldouble a;

	if (pop(&a)) {
		if (a >= 0.0) {
			push(sqrtl(a));
			lastx = a;
			return GOODOP;
		} else {
			push(a);
			printf(" can't take root of negative\n");
			return BADOP;
		}
	}
	return BADOP;
}

opreturn
trig_no_sense(void)
{
	printf(" trig functions make no sense in integer mode");
	return BADOP;
}

opreturn
sine(token *t)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push(sinl((a * pi) / 180.0));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
asine(token *t)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push((180.0 * asinl(a)) / pi);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
cosine(token *t)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push(cosl((a * pi) / 180.0));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
acosine(token *t)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push(180.0 * acosl(a) / pi);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
tangent(token *t)
{
	ldouble a, ta;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		// FIXME:  tan() goes infinite at +/-90
		push(tanl(a * pi / 180.0));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
atangent(token *t)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push((180.0 * atanl(a)) / pi);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
fraction(token *t)
{
	ldouble a;

	if (pop(&a)) {
		if (a > 0)
			push(a - floorl(a));
		else
			push(a - ceill(a));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
integer(token *t)
{
	ldouble a;

	if (pop(&a)) {
		if (a > 0)
			push(floorl(a));
		else
			push(ceill(a));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
clear(token *t)
{
	ldouble scrap;

	if (pop(&lastx)) {
		while (pop(&scrap));
	}
	return GOODOP;
}

opreturn
rolldown(token *t)		// "pop"
{
	(void)pop(&lastx);
	return GOODOP;
}

opreturn
enter(token *t)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		push(a);
		return GOODOP;
	}
	return BADOP;
}

opreturn
repush(token *t)		// "lastx"
{
	push(lastx);
	return GOODOP;
}

opreturn
exchange(token *t)
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
		if (punct && i >= 1)
			putchar(',');
	}
}

void
putbinary(long long n)
{
	if (mode == 'f')	// no masking in float mode
		putbinary2(max_int_width, ~0, (unsigned long long)n);
	else
		putbinary2(int_width, int_mask, (unsigned long long)n);
}

void
puthex(unsigned long long n)
{
	/* commas every 4 hex digits */
	if (n < 0x10000) {
		printf("%Lx", n);
		return;
	}
	puthex((n / 0x10000) );
	if (punct)
		putchar(',');
	printf("%04Lx", n % 0x10000);
}

void
putoct(unsigned long long n)
{
	/* commas every 3 octal digits */
	if (n < 01000) {
		printf("%Lo", n);
		return;
	}
	putoct(n / 01000);
	if (punct)
		putchar(',');
	printf("%03Lo", n % 01000);
}

void
printtop(void)
{
	ldouble n;
	long long ln;
	long long mask = int_mask;
	long long signbit;

	if (mode == 'f')	// no masking in float mode
		mask = ~0;

	if (pop(&n)) {
		switch (print_format) {
		case 'h':
			ln = (long long)n & mask;
			printf(" 0x");
			puthex(ln);
			putchar('\n');
			break;
		case 'o':
			ln = (long long)n & mask;
			printf(" 0");
			putoct(ln);
			putchar('\n');
			break;
		case 'b':
			ln = (long long)n & mask;
			printf(" 0b");
			putbinary(ln);
			putchar('\n');
			break;
		case 'd':
			ln = (long long)n;
			if (mode == 'f' || int_width == LONGLONG_BITS) {
				printf(punct ? " %'Ld\n" : " %Ld\n", ln);
			} else {
				/* shenanigans to make pos/neg numbers
				 * appear properly.
				 */
				long long t;

				signbit = 1LL << (int_width - 1);
				mask = (long long)int_mask & ~signbit;
				if (ln & signbit) {	// negative
					t = signbit - (ln & mask);
					printf(" -");
				} else {
					t = ln & mask;
					printf(" ");
				}
				printf(punct ? "%'Ld\n" : "%Ld\n", t);
			}
			break;
		default:	// 'f'
			printf(punct ? " %'.*Lg\n" : " %.*Lg\n", float_digits,
			       n);
			break;
		}
		push(n);
	}
}

void
printstack(void)
{
	ldouble n;

	if (pop(&n)) {
		(void)printstack();
		push(n);
		printtop();
	}
}

opreturn
printall(token *t)
{
	ldouble hold;

	suppress_autoprint = TRUE;
	empty_stack_ok = TRUE;
	printstack();
	return GOODOP;
}

opreturn
printone(token *t)
{
	suppress_autoprint = TRUE;
	empty_stack_ok = TRUE;
	printtop();
	return GOODOP;
}

opreturn
printhex(token *t)
{
	print_format = 'h';
	printone(t);
	return GOODOP;
}

opreturn
printoct(token *t)
{
	print_format = 'o';
	printone(t);
	return GOODOP;
}

opreturn
printbin(token *t)
{
	print_format = 'b';
	printone(t);
	return GOODOP;
}

opreturn
printdec(token *t)
{
	print_format = 'd';
	printone(t);
	return GOODOP;
}

opreturn
printfloat(token *t)
{
	print_format = 'f';
	printone(t);
	return GOODOP;
}

/* debug support -- hidden command */
opreturn
printraw(token *t)
{
	struct num *s;

	printf("int_mask 0x%llx, int_sign_bit 0x%llx\n", int_mask,
	       int_sign_bit);

	printf("stack:\n");
	s = stack;
	printf("%16s   %16s\n", "(long long)", "(long double)");
	while (s) {
		printf("%#16Lx   %#16Lg\n", (long long)(s->val), s->val);
		s = s->next;
	}
	printf("native sizes (bits):\n");
	printf("%16lu   %16lu\n", 8 * sizeof(long long),
	       8 * sizeof(long double));
	printf("long double mantissa width %d\n", LDBL_MANT_DIG);

	suppress_autoprint = TRUE;
	return GOODOP;
}

opreturn
punctuation(token *t)
{
	punct = !punct;
	printf(" numeric punctuation is now %s\n", punct ? "on" : "off");
	return GOODOP;
}

static char *
mode2name(void)
{
	switch (mode) {
	case 'f':
		return "float";
	case 'd':
		return "decimal";
	case 'o':
		return "octal";
	case 'h':
		return "hex";
	case 'b':
		return "binary";
	default:
		printf(" mode is 0x%x\n", mode);
		return "ERROR";
	}
}

void
showmode(void)
{
	printf(" Mode is %s. ", mode2name());
	if (mode == 'f') {
		printf(" Displaying %d decimal places.\n", float_digits);
	} else {
		printf(" Integer math with %d bits.\n", int_width);
	}
	suppress_autoprint = TRUE;
}

opreturn
modeinfo(token *t)
{
	showmode();
	return GOODOP;
}

opreturn
modehex(token *t)
{
	print_format = mode = 'h';
	showmode();
	return printall(t);	/* side-effect: stack truncated to integer */
}

opreturn
modebin(token *t)
{
	print_format = mode = 'b';
	showmode();
	return printall(t);	/* side-effect: stack truncated to integer */
}

opreturn
modeoct(token *t)
{
	print_format = mode = 'o';
	showmode();
	return printall(t);	/* side-effect: stack truncated to integer */
}

opreturn
modedec(token *t)
{
	print_format = mode = 'd';
	showmode();
	return printall(t);	/* side-effect: stack truncated to integer */
}

opreturn
modefloat(token *t)
{
	print_format = mode = 'f';
	showmode();
	return printall(t);
}

opreturn
precision(token *t)
{
	ldouble digits;

	if (!pop(&digits))
		return BADOP;
	float_digits = digits;
	printf(" %d digits of displayed precision.\n", float_digits);
	return GOODOP;
}

void
setup_width(int bits)
{
	/* we use long double to store our data.  in integer mode, this
	 * means the FP mantissa, if it's shorter, may limit our maximum
	 * word width.
	 */
	if (!max_int_width) {	/* first call */
		max_int_width = LONGLONG_BITS;
		if (max_int_width > LDBL_MANT_DIG)
			max_int_width = LDBL_MANT_DIG;
		bits = max_int_width;
	}

	if (bits > max_int_width)
		bits = max_int_width;

	int_width = bits;
	int_sign_bit = (1LL << (int_width - 1));

	if (int_width == LONGLONG_BITS)
		int_mask = ~0;
	else
		int_mask = (1LL << int_width) - 1;
}

opreturn
width(token *t)
{
	ldouble n;
	long long bits;

	if (!pop(&n))
		return BADOP;

	bits = n;
	if (bits > max_int_width) {
		bits = max_int_width;
		printf(" Width out of range, set to max (%Ld)\n", bits);
	}
	if (bits < 8) {
		bits = 8;
		printf(" Width out of range, set to min (%Ld)\n", bits);
	}

	setup_width(bits);

	printf(" Words are %d bits wide.", int_width);
	if (mode == 'f')
		printf("  (Ignored in float mode!)");
	putchar('\n');

	return printall(t);
}

opreturn
store(token *t)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		offstack = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
store2(token *t)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		offstack2 = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
store3(token *t)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		offstack3 = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
recall(token *t)
{
	push(offstack);
	return GOODOP;
}

opreturn
recall2(token *t)
{
	push(offstack2);
	return GOODOP;
}

opreturn
recall3(token *t)
{
	push(offstack3);
	return GOODOP;
}

opreturn
push_pi(token *t)
{
	push(pi);
	return GOODOP;
}

opreturn
units_in_mm(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a *= 25.4;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_mm_in(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a /= 25.4;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_F_C(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a -= 32.0;
		a /= 1.8;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_C_F(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a *= 1.8;
		a += 32.0;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_l_qt(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a *= 1.05669;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_qt_l(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a /= 1.05669;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_oz_g(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a *= 28.3495;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_g_oz(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a /= 28.3495;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_mi_km(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a /= 0.6213712;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_km_mi(token *t)
{
	ldouble a;

	if (pop(&a)) {
		a *= 0.6213712;
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
autop(token *t)
{
	autoprint = !autoprint;
	printf(" autoprinting is now %s\n", autoprint ? "on" : "off");
	return GOODOP;
}

opreturn
quit(token *t)
{
	exit(0);
}

void
parse_tok(char *p, token *t, char **nextp)
{
	int sign = 1;

	/* be sure + and - are bound closely to numbers */
	if (*p == '+' && (*(p + 1) == '.' || isdigit(*(p + 1)))) {
		p++;
	} else if (*p == '-' && (*(p + 1) == '.' || isdigit(*(p + 1)))) {
		sign = -1;
		p++;
	}

	if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X')) {
		// hex
		long long ln = strtoull(p, nextp, 16);

		if (ln == 0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		debug(("parsed 0x%Lx\n", ln));
		t->val.val = ln * sign;
		return;

	} else if (*p == '0' && (*(p + 1) == 'b' || *(p + 1) == 'B')) {
		// binary
		long long ln = strtoull(p + 2, nextp, 2);

		if (ln == 0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = ln * sign;
		return;

	} else if (*p == '0' && ('0' <= *(p + 1) && *(p + 1) <= '7')) {
		// octal
		long long ln = strtoull(p, nextp, 8);

		if (ln == 0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = ln * sign;
		return;

	} else if (isdigit(*p) || (*p == '.')) {
		// decimal
		long double dd = strtod(p, nextp);

		if (dd == 0.0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = dd * sign;
		return;
	} else {
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
			if (!strncmp(op->name, p, matchlen)) {
				if (p[matchlen] == '\0' || isspace(p[matchlen])) {
					*nextp = p + matchlen;
					t->type = OP;
					t->val.oper = op;
					return;
				}
			}
			op++;
		}
		if (!op->name) {
		      unknown:
			t->val.str = p;
			t->type = UNKNOWN;
			return;
		}
	}
}

static char *input_ptr = NULL;

void
flushinput(void)
{
	input_ptr = NULL;
}

void
no_comm(char *cp)
{
	char *ncp;

	/* first eliminate comments */
	if ((ncp = strchr(cp, '#')) != NULL)
		*ncp = '\0';

	/* then eliminate commas from numbers, like "1,345,011".  this
	 * removes them from the whole line; a side-effect is that
	 * there can be no commas in commands.
	 */
	while ((cp = strchr(cp, ',')) != NULL)
		memmove(cp, cp + 1, strlen(cp));
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

	/* if there are args on the command line, they're taken as
	 * initial commands.  since only numbers can start with '-',
	 * any other use of a hyphen brings up a usage message.
	 */
	if (arg < g_argc) {

		if (g_argv[1][0] == '-' && !(isdigit(g_argv[1][1])))
			usage();

		blen = 0;
		for (arg = 1; arg < g_argc; arg++)
			blen += strlen(g_argv[arg]) + 2;

		if ((input_buf = malloc(blen)) == NULL) {
			perror("malloc");
			exit(1);
		}

		*input_buf = '\0';
		for (arg = 1; arg < g_argc; arg++) {
			strcat(input_buf, g_argv[arg]);
			strcat(input_buf, " ");
		}

		printf("%s\n", input_buf);

		no_comm(input_buf);

		input_ptr = input_buf;
		return 1;
	}
#ifdef USE_READLINE

	static char readline_init_done = 0;

	if (!readline_init_done) {	// readline initializations

		using_history();

		/* prevent readline doing tab filename completion */
		rl_bind_key_in_map('\t', rl_insert,
				   rl_get_keymap_by_name("emacs"));
		rl_bind_key_in_map('\t', rl_insert,
				   rl_get_keymap_by_name("vi-insert"));

		readline_init_done = 1;
	}

	/* if we used the buffer as input, add it to history.  doing
	 * this here records any command line input, possibly stored
	 * in the buffer above, on the first call to fetch_line()
	 */
	if (input_buf && *input_buf)
		add_history(input_buf);

	if ((input_buf = readline("")) == NULL)
		exit(0);

	// readline() doesn't echo bare newlines to tty, so do it here,
	if (*input_buf == '\0')
		putchar('\n');

#else
	if (getline(&input_buf, &blen, stdin) < 0)
		exit(0);

	/* if stdin is a terminal, the command is already on-screen.
	 * but we also want it mixed with the output if we're
	 * redirecting from a file or pipe.  (easy to get rid of it
	 * with something like: "ca < commands | grep '^ '"
	 */
	if (!isatty(0))
		printf("%s", input_buf);

#endif

	no_comm(input_buf);

	input_ptr = input_buf;

	return 1;
}

int
gettoken(struct token *t)
{

	if (input_ptr == NULL)
		if (!fetch_line())
			return 0;

	while (isspace(*input_ptr))
		input_ptr++;

	if (*input_ptr == '\0') {	/* out of input */
		t->type = EOL;
		input_ptr = NULL;
		return 1;
	}

	parse_tok(input_ptr, t, &input_ptr);

	fflush(stdin);
	return 1;
}

opreturn
help(token *t)
{
	oper *op;

	op = opers;
	printf("\
Entering a number pushes it on the stack.\n\
Operators replace either one or two top stack values with their result.\n\
All whitespace is equal; numbers and operators may appear on one or more lines.\n\
Whitespace is optional between numbers and commands, but not vice versa.\n\
Commas can appear in numbers (e.g., \"3,577,455\").\n\
Numbers are represented as long double and signed long long.\n\
Always use 0xNNN/0NNN to enter hex/octal, even in hex or octal mode.\n\
Below, 'x' refers to top-of-stack, 'y' refers to the next value beneath.\n\
\n\
");
	while (op->name) {
		if (!*op->name) {
			putchar('\n');
		} else {
			if (op->name[0] == ';') {
				/* hidden command, for debug */ ;
			} else if (!op->func) {
				printf("%s\n", op->name);
			} else if (!op->help) {
				printf(" %s,", op->name);
			} else {
				printf(" %s\t-- %s\n", op->name, op->help);
			}
		}
		op++;
	}
	printf("%78s\n", __FILE__ " built " __DATE__ " " __TIME__);
	printf
	    ("\nTip:  To see this help in a pager, try running \"ca help q | less\"\n");
	return GOODOP;
}

// *INDENT-OFF*.
struct oper opers[] = {
    {"Operators with two operands", 0, 0},
	{"+", add,		0 },
	{"-", subtract,		"Add and subtract x and y" },
	{"*", multiply,		0 },
	{"x", multiply,		"Two ways to multiply x and y" },
	{"/", divide,		0 },
	{"%", modulo,		"Divide and modulo of y by x (arithmetic shift)" },
	{"^", y_to_the_x,	"Raise y to the x'th power" },
	{">>", rshift,		0 },
	{"<<", lshift,		"Right/left logical shift of y by x bits" },
	{"&", and,		0 },
	{"|", or,		0 },
	{"xor", xor,		"Bitwise AND, OR, and XOR of y and x" },
	{"setb", setbit,	0 },
	{"clearb", clearbit,	"Set and clear shift bit x in y" },
	{"", 0, 0},
    {"Operators with one operand", 0, 0},
	{"~", not,		"Bitwise NOT of x (1's complement)" },
	{"chs", chsign,		0 },
	{"negate", chsign,	"Change sign of x (2's complement)" },
	{"recip", recip,        0 },
	{"sqrt", squarert,      "Reciprocal and square root of x" },
	{"sin", sine,           0 },
	{"cos", cosine,         0 },
	{"tan", tangent,        0 },
	{"asin", asine,         0 },
	{"acos", acosine,       0 },
	{"atan", atangent,      "Trig functions (in degrees)" },
	{"abs", absolute,	0 },
	{"frac", fraction,	0 },
	{"int", integer,	"Absolute value, fractional and integer parts of x" },
	{"", 0, 0},
    {"Stack manipulation", 0, 0},
	{"clear", clear,	"Clear stack" },
	{"pop", rolldown,	"Pop (and discard) x" },
	{"push", enter,		0 },
	{"dup", enter,		0 },
	{"enter", enter,	"Push (duplicate) x" },
	{"lastx", repush,	0 },
	{"lx", repush,		"Fetch previous x" },
	{"exch", exchange,	0 },
	{"swap", exchange,	"Exchange x and y" },
	{"store", store,	0 },
	{"sto", store,		0 },
	{"sto1", store,		0 },
	{"sto2", store2,	0 },
	{"sto3", store3,	"Save x off-stack (3 locations)" },
	{"recall", recall,	0 },
	{"rcl", recall,		0 },
	{"rcl1", recall,	0 },
	{"rcl2", recall2,	0 },
	{"rcl3", recall3,	"Fetch x (3 locations)" },
	{"pi", push_pi,		"Push constant pi" },
	{"", 0, 0},
    {"Conversions:", 0, 0},
	{"i2mm", units_in_mm,   0 },
	{"mm2i", units_mm_in,   "inches / millimeters" },
	{"f2c", units_F_C,      0 },
	{"c2f", units_C_F,      "degrees F/C" },
	{"oz2g", units_oz_g,    0 },
	{"g2oz", units_g_oz,    "ounces / grams" },
	{"q2l", units_qt_l,     0 },
	{"l2q", units_l_qt,     "quarts / liters" },
	{"mi2km", units_mi_km,  0 },
	{"km2mi", units_km_mi,  "miles / kilometers" },
	{"", 0, 0},
    {"Display:", 0, 0},
	{"P", printall,		"Print whole stack" },
	{"p", printone,		"Print x in mode's format" },
	{"f", printfloat,	0 },
	{"d", printdec,		0 },
	{"o", printoct,		0 },
	{"h", printhex,		0 },
	{"b", printbin,		"Print x in float, decimal, octal, hex, binary" },
	{";r", printraw,	"actual stack contents, for debug (hidden)" },
	{"autoprint", autop,	0 },
	{"a", autop,		"Toggle autoprinting on/off" },
	{"", 0, 0},
    {"Modes:", 0, 0},
	{"F", modefloat,	"Switch to floating point mode" },
	{"D", modedec,		0 },
	{"I", modedec,		"Switch to decimal integer mode" },
	{"H", modehex,		0 },
	{"X", modehex,		"Switch to hex mode" },
	{"O", modeoct,		0 },
	{"B", modebin,		"Switch to octal or binary modes" },
	{"precision", precision, 0 },
	{"k", precision,        "Set float mode display precision" },
	{"width", width,	0 },
	{"w", width,		"Set effective \"word size\" for integer modes" },
	{"commas", punctuation,	0 },
	{"c", punctuation,	"Toggle comma separators in numbers on/off" },
	{"mode", modeinfo,	"Display current mode parameters" },
	{"", 0, 0},
    {"Housekeeping:", 0, 0},
	{"?", help,		0 },
	{"help", help,		"this list" },
	{"quit", quit,		0 },
	{"q", quit,		0 },
	{"exit", quit,		"leave" },
	{"#", help,		"Comment. The '#' and the rest of the line will be ignored." },
	{NULL, NULL},
};
// *INDENT-ON*.

int
main(argc, argv)
int argc;
char *argv[];
{
	struct token tok;
	token *t = &tok;
	static int lasttoktype;
	char *pn;

	pn = strrchr(argv[0], '/');
	progname = pn ? (pn + 1) : argv[0];

	/* fetch_line() will process arguments as commands directly */
	g_argc = argc;
	g_argv = argv;

	// apparently needed to make the %'Ld format for commas work
	setlocale(LC_ALL, "");

	setup_width(0);

	/* we simply loop forever, either pushing operands or
	 * executing operators.  the special end-of-line token lets us
	 * do reasonable autoprinting, if the last thing on the line
	 * was an operator.
	 */
	while (1) {
		if (!gettoken(t))
			break;

		print_format = mode;
		switch (t->type) {
		case NUMERIC:
			push(t->val.val);
			break;
		case OP:
			(void)(t->val.oper->func) (t);
			break;
		case EOL:
			if (!suppress_autoprint && autoprint
			    && lasttoktype == OP) {
				empty_stack_ok = TRUE;
				printtop();
			}
			suppress_autoprint = FALSE;
			break;
		default:
		case UNKNOWN:
			printf(" unrecognized input '%s'\n", t->val.str);
			flushinput();
			break;

		}
		empty_stack_ok = FALSE;
		lasttoktype = t->type;
	}
	exit(1);
	return (1);
}
