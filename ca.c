/* 
 *  build with:
 *	doit: gcc -g -o ca -D HAVE_READLINE ca.c -lm -lreadline
 *	doit-x: gcc -g -o ca ca.c -lm
 *
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
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <locale.h>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

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

/* who are we? */
char *progname;

ldouble pi = 3.141592653589793238462643383279502884L;

/* internal representation of numbers */
struct num {
	ldouble val;
	struct num *next;
};

/* all user input is either a number or a command operator --
 * this is how the operators are looked up, by name
 */
typedef struct token token;
typedef struct oper oper;
struct oper {
	char *name;
	opreturn (*func)(token *);
	char *help;
	int no_sign_extend;  /* strip sign extension for this command */
};

/* tokens are typed objects -- currently numbers, operators, line-ends */
struct token {
	union {
		ldouble val;
		// opreturn (*opfunc)(struct token *);
		oper *oper;
		char *str;
	} val;
	int type;
};

/* values for token.type */
#define FLOAT 0
#define OP 1
#define EOL 2
#define UNKNOWN -1


/* the operand stack */
struct num *stack;

/* if true, print the top of stack after line that ends with an operator */
boolean autoprint = TRUE;
boolean suppress_autoprint = FALSE;

/* floating point precision */
int float_digits = 6;

/* integer word display width in bits */
#define NATIVE_WIDTH (sizeof(long long) * 8)

int int_width = NATIVE_WIDTH;
long long int_sign_bit = (1LL << (NATIVE_WIDTH - 1));
long long int_mask = ~0;

/* 4 modes: float, decimal integer, hex, and octal.
 * last 3 are integer modes.
 */
int mode = 'f';  /* or 'd', 'x', or 'o' */

/* decimal, hex, or octal output.  normally matches mode, but
 * can be changed by individual commands
 */
int print_format = 'f';  /* 'f', 'd', 'x', or 'o' */

/* when working in a non-native word width, many operators don't
 * want sign extension.
 */
int no_sign_extend;

/* used to suppress "empty stack" messages */
boolean empty_stack_ok = FALSE;

/* the most recent top-of-stack */
ldouble lastx;

/* for store/recall */
ldouble offstack;
ldouble offstack2;
ldouble offstack3;

/* operator table */
struct oper opers[];

void parse_tok(char *p, token *t, char **nextp);

void
errexit(char *s)
{
	fprintf(stderr, "%s: %s\n", progname, s);
	exit(1);
}

long long
sign_extend(ldouble a)
{
    long long b = a;
    if (int_width == NATIVE_WIDTH)
	    return b;
    else
	    return b | (0 - (b & int_sign_bit));
}

void
push(ldouble n)
{
	struct num *p = (struct num *)calloc(1, sizeof (struct num));
	if (!p) errexit("no memory for push");
	if (mode != 'f') {
		p->val = sign_extend((long long)n & int_mask);
		debug(("push stored m/e 0x%Lx\n", (long long)(p->val)));
	} else {
		p->val = n;
		debug(("push stored 0x%Lx as-is\n", (long long)(p->val)));
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
			printf("empty stack\n");
		return FALSE;
	}
	if (no_sign_extend) {
		*f = (long long)(p->val) & int_mask;
		debug(("pop returned masked 0x%Lx\n", (long long)*f));
	} else {
		*f = p->val;
		debug(("pop returned 0x%Lx as-is\n", (long long)*f));
	}
	stack = p->next;
	free (p);
	return TRUE;
}

opreturn
add( token *t )
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
subtract( token *t )
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
multiply( token *t )
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
divide( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			if (b != 0.0) {
				push(a / b);
			} else {
				push(a);
				push(b);
				printf("would divide by zero\n");
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
modulus( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
			i = (long long)a;
			j = (long long)b;
			if (j != 0) {
				push(i / j);
			} else {
				push(a);
				push(b);
				printf("would divide by zero\n");
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
y_to_the_x ( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			if (a >= 0 || floorl(b) == b) {
				push(powl(a, b));
			} else {
				push(a);
				push(b);
				printf("result would be complex\n");
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
rshift( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
			i = (long long)a;
			j = (long long)b;
			push(i >> j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
lshift( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
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
and( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
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
or( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
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
xor( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
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
not( token *t )
{
	ldouble a;
	if (pop(&a)) {
		push( ~(long long)a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
chsign( token *t )
{
	ldouble a;
	if (pop(&a)) {
		push( -a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
recip ( token *t )
{
	ldouble a;
	if (pop(&a)) {
		if (a != 0.0) {
			push( 1.0 / a );
			lastx = a;
			return GOODOP;
		} else {
			push(a);
			printf("would divide by zero\n");
			return BADOP;
		}
	}
	return BADOP;
}

opreturn
squarert ( token *t )
{
	ldouble a;
	if (pop(&a)) {
		if (a >= 0.0) {
			push( sqrtl(a) );
			lastx = a;
			return GOODOP;
		} else {
			push(a);
			printf("can't take root of negative\n");
			return BADOP;
		}
	}
	return BADOP;
}

opreturn
trig_no_sense(void)
{
    printf("trig functions make no sense in integer mode");
    return BADOP;
}

opreturn
sine ( token *t )
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
asine ( token *t )
{
	ldouble a;

	if (mode != 'f')
	    return trig_no_sense();

	if (pop(&a)) {
		push( (180.0 * asinl(a)) / pi  );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
cosine ( token *t )
{
	ldouble a;

	if (mode != 'f')
	    return trig_no_sense();

	if (pop(&a)) {
		push( cosl((a * pi) / 180.0 ) );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
acosine ( token *t )
{
	ldouble a;

	if (mode != 'f')
	    return trig_no_sense();

	if (pop(&a)) {
		push( 180.0 * acosl(a) / pi );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
tangent ( token *t )
{
	ldouble a, ta;

	if (mode != 'f')
	    return trig_no_sense();

	if (pop(&a)) {
		// FIXME:  tan() goes infinite at +/-90 
		push( tanl(a * pi / 180.0 ) );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
atangent ( token *t )
{
	ldouble a;

	if (mode != 'f')
	    return trig_no_sense();

	if (pop(&a)) {
		push( (180.0 * atanl(a)) / pi );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
fraction ( token *t )
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
integer ( token *t )
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
clear ( token *t )
{
	ldouble scrap;
	if (pop(&lastx)) {
		while (pop(&scrap))
			;
	}
	return GOODOP;
}

opreturn
rolldown ( token *t ) // "pop"
{
	(void) pop(&lastx);
	return GOODOP;
}

opreturn
enter( token *t )
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
repush ( token *t ) // "lastx"
{
	push(lastx);
	return GOODOP;
}

opreturn
exchange( token *t )
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
printtop (void)
{
	ldouble n;
	long long ln;
	if (pop(&n)) {
		switch (print_format) {
		case 'x':
			ln = (long long)n & int_mask;
			printf(" 0x%Lx\n",ln);
			break;
		case 'o':
			ln = (long long)n & int_mask;
			printf(" 0%Lo\n",ln);
			break;
		case 'd':
			ln = (long long)n & int_mask;
			printf(" %'Ld\n",ln);
			break;
		default: // 'f'
			printf(" %.*Lg\n",float_digits,n);
			break;
		}
		push(n);
	}
}

void
printstack (void)
{
	ldouble n;
	if (pop(&n)) {
		(void)printstack();
		push(n);
		printtop();
	}
}

opreturn
printall ( token *t )
{
	ldouble hold;
	suppress_autoprint = TRUE;
	empty_stack_ok = TRUE;
	printstack();
	return GOODOP;
}

opreturn
printone ( token *t )
{
	suppress_autoprint = TRUE;
	empty_stack_ok = TRUE;
	printtop();
	return GOODOP;
}

opreturn
printhex ( token *t )
{
	print_format = 'x';
	printone(t);
	return GOODOP;
}

opreturn
printoct ( token *t )
{
	print_format = 'o';
	printone(t);
	return GOODOP;
}

opreturn
printdec ( token *t )
{
	print_format = 'd';
	printone(t);
	return GOODOP;
}

opreturn
printfloat ( token *t )
{
	print_format = 'f';
	printone(t);
	return GOODOP;
}

static char *
mode2name(void)
{
    switch (mode) {
    case 'f':  return "float"; break;
    case 'd':  return "decimal integer"; break;
    case 'o':  return "octal"; break;
    case 'x':  return "hex"; break;
    default:  printf("mode is 0x%x\n", mode); return "ERROR"; break;
    }
}

void
showmode(void)
{
    printf("Current mode is %s.\n", mode2name());
    if (mode == 'f') {
	printf("displayed precision is %d decimal places.\n", float_digits);
    } else {
	printf("displayed word width is %d bits.\n", int_width);
	printf("fractions will be truncated.\n");
    }
}

opreturn
modeinfo ( token *t )
{
	showmode();
	return GOODOP;
}

opreturn
modehex ( token *t )
{
	print_format = mode = 'x';
	showmode();
	return printall(t);  /* side-effect:  whole stack truncated */
}

opreturn
modeoct ( token *t )
{
	print_format = mode = 'o';
	showmode();
	return printall(t);  /* side-effect:  whole stack truncated */
}

opreturn
modedec ( token *t )
{
	print_format = mode = 'd';
	showmode();
	return printall(t);  /* side-effect:  whole stack truncated */
}

opreturn
modefloat ( token *t )
{
	print_format = mode = 'f';
	showmode();
	return printall(t);
}

opreturn
precision ( token *t )
{
	ldouble digits;
	if (!pop(&digits))
		return BADOP;
	float_digits = digits;
	printf("%d digits of displayed precision.\n", float_digits);
	return GOODOP;
}

opreturn
width ( token *t )
{
	ldouble bits;
	if (!pop(&bits))
		return BADOP;

	switch ((long long)bits) {
	case NATIVE_WIDTH:
		int_width = bits;
		int_mask = ~0;
		break;
	case 32:
		int_width = bits;
		int_mask = 0xffffffff;
		int_sign_bit = 0x80000000;
		break;
	case 16:
		int_width = bits;
		int_mask = 0xffff;
		int_sign_bit = 0x8000;
		break;
	case 8:
		int_width = bits;
		int_mask = 0xff;
		int_sign_bit = 0x80;
		break;
	default:
		printf("currently bits must be 8, 16, 32, or %ld\n",
			NATIVE_WIDTH);
		push(bits);
		return BADOP;
	}
	printf("Using %d bit integers.\n", int_width);
	return printall(t);
}

opreturn
store ( token *t )
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
store2 ( token *t )
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
store3 ( token *t )
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
recall ( token *t )
{
	push(offstack);
	return GOODOP;
}

opreturn
recall2 ( token *t )
{
	push(offstack2);
	return GOODOP;
}

opreturn
recall3 ( token *t )
{
	push(offstack3);
	return GOODOP;
}

opreturn
push_pi ( token *t )
{
	push(pi);
	return GOODOP;
}

opreturn
units_in_mm( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a *= 25.4;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_mm_in( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a /= 25.4;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_F_C( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a -= 32.0;
		a /= 1.8;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_C_F( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a *= 1.8;
		a += 32.0;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_l_qt( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a *= 1.05669;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_qt_l( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a /= 1.05669;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_oz_g( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a *= 28.3495;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_g_oz( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a /= 28.3495;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_mi_km( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a *= 1.60934;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_km_mi( token *t )
{
	ldouble a;
	if (pop(&a)) {
		a /= 1.60934;
		push( a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
autop ( token *t )
{
	autoprint = !autoprint;
	printf("autoprinting is now %s\n", autoprint ? "on" : "off");
	return GOODOP;
}

opreturn
quit ( token *t )
{
	exit(0);
}

void parse_tok(char *p, token *t, char **nextp)
{
	int sign = 1;

	/* be sure + and - are bound closely to numbers */
	if (*p == '+' && (*(p+1) == '.' || isdigit(*(p+1)))) {
		p++;  
	} else if (*p == '-' && (*(p+1) == '.' || isdigit(*(p+1)))) {
		sign = -1;
		p++;  
	}

	if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) {
		// hex
		long long ln = strtoll(p, nextp, 16);
		t->type = FLOAT;
		t->val.val = ln * sign;
		return;

	} else if (*p == '0' && ('0' <= *(p+1) && *(p+1) <= '7')) {
		// octal
		long long ln = strtoll(p, nextp, 8);
		t->type = FLOAT;
		t->val.val = ln * sign;
		return;

	} else if (isdigit(*p) || (*p == '.')) {
		// decimal
		long double dd = strtod(p, nextp);
		t->type = FLOAT;
		t->val.val = dd * sign;
		return;
	} else {
		// command
		struct oper *op;
		op = opers;
		while (op->name) {
			int matchlen;
			if (!op->func) {
				op++;
				continue;
			}
			matchlen = strlen(op->name);
			if (!strncmp(op->name, p, matchlen)) {
				if (p[matchlen] == '\0' ||
						isspace(p[matchlen]) ) {
					*nextp = p + matchlen;
					t->type = OP;
					t->val.oper = op;
					return;
				}
			}
			op++;
		}
		if (!op->name) {
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

int
fetch_line(void)
{
#ifndef HAVE_READLINE
	static char inputline[1024];

	if (fgets(inputline, 1024, stdin) == NULL)
		exit(0);

	/* if stdin is a terminal, the command is already on-screen.
	 * we also want it there if we're redirecting from a file or pipe.
	 * (easy to get rid of it with "ca < commands | grep '^ '"
	 */
	if (!isatty(fileno(stdin)))
		printf("%s", inputline);

	inputline[strlen(inputline)-1] = '\0';
	input_ptr = inputline;
#else
	static char *rl_buf;
	static char init_done = 0;

	if (!init_done) {

		using_history();

		/* prevent readline doing tab filename completion
		 * in both emacs and vi-insert maps.
		 */
		rl_bind_key_in_map ('\t', rl_insert,
			rl_get_keymap_by_name("emacs"));
		rl_bind_key_in_map ('\t', rl_insert,
			rl_get_keymap_by_name("vi-insert"));

		init_done = 1;
	}

	if (rl_buf)
		free(rl_buf);

	if ((rl_buf = readline("")) == NULL)
		exit(0);

	// readline doesn't echo bare newlines to tty, so do it here,
	if (*rl_buf == '\0')
		putchar('\n');
	else // but only add non-null lines to command history
		add_history(rl_buf);


	input_ptr = rl_buf;
#endif


	/* eliminate commas from numbers:  "45,001" we eliminate from
	 * the whole line, so there can be no commas in commands.
	 */
	char *cp = input_ptr;
	while ((cp = strchr(cp, ',')) != NULL) {
		memmove(cp, cp+1, strlen(cp));
	}

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

	debug(("gettoken string is '%s'\n", input_ptr));

	if (*input_ptr == '\0') { /* out of input */
		t->type = EOL;
		input_ptr = NULL;
		debug(("gettoken returning EOL\n"));
		return 1;
	}

	debug(("gettoken parsing input_ptr == %s\n", input_ptr));
	parse_tok(input_ptr, t, &input_ptr);
	debug(("gettoken input_ptr is now %s\n", input_ptr));

	fflush(stdin);
	return 1;
}

opreturn
help ( token *t )
{
	struct oper *op;
	op = opers;
	printf( "\
Entering a number pushes it on the stack.\n\
Operators replace either one or two top stack values with their result.\n\
All whitespace is equal; numbers and operators may appear on one or more lines.\n\
Whitespace is optional between numbers and commands, but not vice versa.\n\
Commas can appear in numbers (e.g., \"3,577,455\").\n\
Math is done in long double floating point, which is preserved in float mode.\n\
Hex, octal, and decimal integer modes convert results to long long integer.\n\
Use 0xNNN/0NNN to enter hex/octal, even when in hex or octal mode.\n\
Below, 'x' refers to top-of-stack, 'y' refers to the next value beneath.\n\
\n\
");
	while (op->name) {
		if (!*op->name) {
			putchar('\n');
		} else {
#if BEFORE
			if (!op->func)
				printf("%s\n", op->name);
			else if (op->help)
				printf("   %-20s%s\n", op->name, op->help);
			else
				printf("     or %-15s%s\n", op->name, " \" \"");
#else
			if (!op->func) {
				printf("%s\n", op->name);
			} else if (!op->help) {
				printf(" %s,", op->name);
			} else {
				printf(" %s\t-- %s\n", op->name, op->help);
			}
#endif
		}
		op++;
	}
	// putchar('\n');
	// showmode();
	// printf("autoprinting is %s\n", autoprint ? "on" : "off");
	printf("%78s\n", __FILE__" built "__DATE__" "__TIME__);
	return GOODOP;
}


struct oper opers[] = {
    {"Operators with two operands", 0, 0},
	{"+", add, 		0 }, 
	{"-", subtract, 	"Add and subtract x and y" },
	{"*", multiply,		0, 1},
	{"x", multiply,		"Two ways to multiply x and y" },
	{"/", divide, 		0 },
	{"%", modulus, 		"Divide and modulo of y by x" },
	{"^", y_to_the_x, 	"Raise y to the x'th power" },
	{">>", rshift, 		0 },
	{"<<", lshift, 		"Right and left shift of y by x bits", 1 },
	{"&", and, 		0 },
	{"|", or, 		0 },
	{"xor", xor, 		"Bitwise AND, OR, and XOR of y and x", 1 },
	{"", 0, 0},
    {"Operators with one operand", 0, 0},
	{"~", not, 		"Bitwise NOT of x", 1 },
	{"chs", chsign,		0 },
	{"negate", chsign,	"Negate x" },
	{"recip", recip,        "Reciprocal of x" },
	{"sqrt", squarert,      "Square root of x" },
	{"sin", sine,           0 },
	{"cos", cosine,         0 },
	{"tan", tangent,        "Sine, cosine, tangent of angle x in degrees" },
	{"asin", asine,         0 },
	{"acos", acosine,       0 },
	{"atan", atangent,      "Arcsine, arccosine, arctangent of x" },
	{"frac", fraction,	0 },
	{"int", integer,	"Fractional and integral parts of x" },
	{"", 0, 0},
    {"Stack manipulation", 0, 0},
	{"clear", clear, 	"Clear stack" },
	{"pop", rolldown, 	"Pop (and discard) x", 1 },
	{"push", enter, 	0 },
	{"enter", enter, 	"Push (duplicate) x" },
	{"lastx", repush, 	0 },
	{"lx", repush, 		"Fetch previous x" },
	{"exch", exchange,	0, 1 },
	{"swap", exchange, 	"Exchange x and y", 1 },
	{"store", store, 	0 },
	{"sto", store,		0 },
	{"sto1", store,		0 },
	{"sto2", store2,	0 },
	{"sto3", store3,	"Save x (3 places)" },
	{"recall", recall, 	0 },
	{"rcl", recall, 	0 },
	{"rcl1", recall, 	0 },
	{"rcl2", recall2, 	0 },
	{"rcl3", recall3, 	"Fetch x (3 places)" },
	{"pi", push_pi, 	"Push constant pi" },
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
	{"P", printall, 	"Print whole stack" },
	{"p", printone, 	"Print x in mode's format" },
	{"f", printfloat, 	0 },
	{"d", printdec, 	0 },
	{"o", printoct, 	0 },
	{"h", printhex, 	0 },
	{"x", printhex, 	"Print x in float, decimal, octal, hex" },
	{"Autoprint", autop,	0 },
	{"A", autop, 		"Toggle autoprinting" },
	{"", 0, 0},
    {"Modes:", 0, 0},
	{"F", modefloat, 	"Switch to floating point mode" },
	{"D", modedec, 		0 },
	{"I", modedec,		"Switch to decimal integer mode" },
	{"H", modehex, 		0 },
	{"X", modehex, 		"Switch to hex mode" },
	{"O", modeoct, 		"Switch to octal mode" },
	{"precision", precision, 0, 1 },
	{"k", precision,        "Set float mode display precision", 1 },
	{"width", width,	"Set integer mode display bits", 1 },
	{"mode", modeinfo,		"Display current mode parameters" },
	{"", 0, 0},
    {"Housekeeping:", 0, 0},
	{"?", help, 		0 },
	{"help", help, 		"this list" },
	{"quit", quit, 		0 },
	{"q", quit, 		0 },
	{"exit", quit, 		"leave" },
	{NULL, NULL},
};

int
main(argc,argv)
int argc;
char *argv[];
{
	struct token tok;
	token *t;
	static int lasttoktype;
	char *pn;

	pn = strrchr(argv[0], '/');
	progname = pn ? (pn + 1) : argv[0];

	if (argc != 1) {
		fprintf(stderr, "%s: no options supported.\n", progname);
		fprintf(stderr, "Type 'help' for command list.\n");
		exit(1);
	}

	setlocale(LC_ALL,"");

	t = &tok;

	/* we simply loop forever, either pushing operands or
	 * executing operators.  the special end-of-line token lets us
	 * do reasonable autoprinting, if the last thing on the line
	 * was an operator
	 */
	while (1) {
		if (!gettoken(t))
			break;
		debug(("got token\n"));
		print_format = mode;
		switch(t->type) {
		case FLOAT:
			debug(("pushing %Lg\n", t->val.val));
			push(t->val.val);
			break;
		case OP:
			debug(("calling op\n"));
			if (t->val.oper->no_sign_extend)
			    no_sign_extend = 1;
			(void)(t->val.oper->func)(t);
			break;
		case EOL:
			if (!suppress_autoprint && autoprint &&
					lasttoktype == OP) {
				empty_stack_ok = TRUE;
				printtop();
			}
			suppress_autoprint = FALSE;
			break;
		default:
		case UNKNOWN:
			printf("unrecognized input '%s'\n",t->val.str);
			flushinput();
			break;

		}
		no_sign_extend = 0;
		empty_stack_ok = FALSE;
		lasttoktype = t->type;
	}
	exit(1);
	return (1);
}

