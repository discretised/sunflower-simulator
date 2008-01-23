/*										*/
/*		Copyright (C) 1999-2006 Phillip Stanley-Marbell.		*/
/*										*/
/*	You may redistribute and/or copy the source and binary			*/
/*	versions of this software provided:					*/
/*										*/
/*	1.	The source retains the copyright notice above, 			*/
/*		this list of conditions, and the following 			*/
/*		disclaimer.							*/
/*										*/
/*	2.	Redistributed binaries must reproduce the above			*/
/*		copyright notice, this list of conditions, and			*/
/*		the following disclaimer.					*/
/*										*/
/*	3.	The source and binaries are used for non-commercial 		*/
/*		purposes only.							*/
/*										*/
/*	4.	Permission is granted to all others, to redistribute		*/
/*		or make copies of the source or binaries, either		*/
/*		as it was received or with modifications, provided		*/
/*		the above three conditions are enforced and 			*/
/*		preserved, and that permission is granted for 			*/
/*		further redistribution, under the conditions of this		*/
/*		document.							*/
/*										*/
/*	The source is provided "as is", and no warranty is either		*/
/*	implied or explicitly granted. The authors will not be liable 		*/
/*	for any damages arising in any way out of the use or misuse 		*/
/*	of this software, even if advised of the possibility of such 		*/
/*	damage.									*/
/*										*/
/*	Contact: phillip Stanley-Marbell <pstanley@ece.cmu.edu>			*/
/*										*/	
#include "sf.h"
#include <stdio.h>
#include <string.h>

/*										*/
/*	This file contains routines to parse a file in the format output	*/
/*	by 'objdump --stabs'.  Binaries must be compiled with the flags		*/
/*	'-O0 -gstabs3' for the STABS debugging information to be output into	*/
/*	executable by GCC. It is particularly important to compile with -O0	*/
/*	as any trickery with the frame pointer (e.g., -fomit-frame-pointer)	*/
/*	will confuse fdr.							*/
/*										*/
/*	If a frame offset or register is used by the compiler for other		*/
/*	purposes in addition to the storage for a variable as known by the	*/
/*	STABS information, we currently do not have a way to distinguish	*/
/*	the authentic values which are those fo the variable, from the		*/
/*	values of other uses of the address or register A possible heuristic	*/
/*	is to check if the value looks like a valid code / data address,	*/
/*	and if the variable is not a pointer, ignore it.			*/
/*										*/

enum
{
	SCOPE_GLOBAL,
	SCOPE_FUNCTION,
};

enum
{
	MAX_NTYPE_LEN = 16,
	MAX_STAB_VARNAMELEN = 64,
	MAX_STAB_TYPENAMELEN = 64,
	MAX_STABSTR_LEN = 128,
	MAX_STABLINE_LEN = 2*128,
};

enum
{
	STAB_STACKVAR,
	STAB_CPLUSNEST,
	STAB_a,
	STAB_b,
	STAB_c,
	STAB_C,
	STAB_d,
	STAB_D,
	STAB_f,
	STAB_F,
	STAB_G,
	STAB_i,
	STAB_I,
	STAB_J,
	STAB_L,
	STAB_m,
	STAB_p,
	STAB_pP,
	STAB_pF,
	STAB_P,
	STAB_Q,
	STAB_R,
	STAB_r,
	STAB_S,
	STAB_s,
	STAB_t,
	STAB_T,
	STAB_v,
	STAB_V,
	STAB_x,
	STAB_X,
};

typedef struct Type Type;
struct Type
{
	int		ispointer;
	char		name[MAX_STAB_TYPENAMELEN];

	/*							*/
	/*	The (filenumber, filetypenumber) of the type	*/
	/*							*/
	int		a;
	int		b;

	Type		*next;
	Type		*alias;
};

typedef struct
{
	int		symnum;
	char		ntype[MAX_NTYPE_LEN];
	int		nothr;
	int		ndesc;
	uvlong		nvalue;
	int		nstrx;

	char		*name;
	Type		type;
	int		descrtype;
} Stab;

static void	parse_stabstr(State *S, char *stabstr, char **name, int *descrtype, Type *type);
static void	parse_type(State *S, char *stabstr, int *a, int *b, int *ispointer);
static void	add_type(State *S, Type type);
static void	reset_types(void);
static char *	lookuptype(int a, int b);
static int	typename2size(char *typename);

Type	*typeshd, *typestl;

/*
static void	dumptypelist(void);

void
dumptypelist(void)
{
	Type	*t = typeshd;

	while (t != NULL)
	{
		printf("\tdumptypelist: %s\n", t->name);

		t = t->next;
	}
}
*/

char *
lookuptype(int a, int b)
{
	Type	*t = typeshd;

	while (t != NULL)
	{
		if (t->a == a && t->b == b)
		{
			return t->name;
		}

		t = t->next;
	}

	return NULL;
}

void
add_type(State *S, Type type)
{
	Type *t;

	t = mmalloc(sizeof(Type), "Type *t in add_type()");
	if (t == NULL)
	{
		sfatal(S, "Malloc failed for Type *t in add_type()");
	}

	*t = type;
	t->next = NULL;
	t->alias = NULL;

	if (typeshd == NULL)
	{
		typeshd = typestl = t;
	}
	else
	{
		typestl->next = t;
		typestl = t;
	}

	return;
}

void
reset_types(void)
{
	Type	*t = typeshd, *d;

	while (t != NULL)
	{
		d = t;
		t = t->next;
		mfree(d, "Type *d in reset_types()");
	}
	typeshd = NULL;
	typestl = NULL;

	return;
}

void
parse_type(State *S, char *stabstr, int *a, int *b, int *ispointer)
{
	*ispointer = 0;

	/*	Skip the initial 't/p/etc'	*/
	if (stabstr[0] != '(')
	{
		stabstr = &stabstr[1];
	}
	if (stabstr == NULL || stabstr[0] != '(')
	{
		sfatal(S, "Badly formatted stabstring in parse_type()");
	}
	sscanf(stabstr, "(%d,%d)", a, b);
	mstrsep(&stabstr, "=");

	while (stabstr != NULL)
	{
		if (stabstr[0] == '*')
		{
			*ispointer = 1;
			stabstr = &stabstr[1];
		}
		else if (stabstr[0] != '(')
		{
			/*	We stop at type aliases / alias pointers	*/

			return;
		}

		if (stabstr == NULL || stabstr[0] != '(')
		{
			sfatal(S, "Badly formatted stabstring in parse_type()");
		}
		sscanf(stabstr, "(%d,%d)", a, b);
		mstrsep(&stabstr, "=");
	}
	//printf("\tparsetype: (a,b) = (%d,%d)\n", *a, *b);

	return;
}

void
parse_stabstr(State *S, char *stabstr, char **name, int *descrtype, Type *type)
{
	/*	Type of the stabstr is determined by the char after the ':'	*/
	*name = mstrsep(&stabstr, ":");

	if (stabstr == NULL || strlen(stabstr) < 1)
	{
		
		*descrtype = -1;
		return;
	}

	parse_type(S, stabstr, &type->a, &type->b, &type->ispointer);
	if ((stabstr[0] >= '0') && (stabstr[0] <= '9'))
	{
		*descrtype = STAB_STACKVAR;
	}
	else switch (stabstr[0])
	{
		case '(':
		case '-':
		{
			*descrtype = STAB_STACKVAR;
			break;
		}
		case ':':
		{
			*descrtype = STAB_CPLUSNEST;
			break;
		}
		case 'a':
		{
			*descrtype = STAB_a;
			break;
		}
		case 'b':
		{
			*descrtype = STAB_b;
			break;
		}
		case 'c':
		{
			*descrtype = STAB_c;
			break;
		}
		case 'C':
		{
			*descrtype = STAB_C;
			break;
		}
		case 'd':
		{
			*descrtype = STAB_d;
			break;
		}
		case 'D':
		{
			*descrtype = STAB_D;
			break;
		}
		case 'f':
		{
			*descrtype = STAB_f;
			break;
		}
		case 'F':
		{
			*descrtype = STAB_F;
			break;
		}
		case 'G':
		{
			*descrtype = STAB_G;
			break;
		}
		case 'i':
		{
			*descrtype = STAB_i;
			break;
		}
		case 'I':
		{
			*descrtype = STAB_I;
			break;
		}
		case 'J':
		{
			*descrtype = STAB_J;
			break;
		}
		case 'L':
		{
			*descrtype = STAB_L;
			break;
		}
		case 'm':
		{
			*descrtype = STAB_m;
			break;
		}
		case 'p':
		{
			*descrtype = STAB_p;
			break;
		}
		case 'P':
		{
			*descrtype = STAB_P;
			break;
		}
		case 'Q':
		{
			*descrtype = STAB_Q;
			break;
		}
		case 'R':
		{
			*descrtype = STAB_R;
			break;
		}
		case 'r':
		{
			*descrtype = STAB_r;
			break;
		}
		case 'S':
		{
			*descrtype = STAB_S;
			break;
		}
		case 's':
		{
			*descrtype = STAB_s;
			break;
		}
		case 't':
		{
			*descrtype = STAB_t;

			/*							*/
			/*	Type will be sticking around after current	*/
			/*	stack frame is gone, so need explicit storage	*/
			/*							*/
			strcpy(type->name, *name);

			break;
		}
		case 'T':
		{
			*descrtype = STAB_T;
			break;
		}
		case 'v':
		{
			*descrtype = STAB_v;
			break;
		}
		case 'V':
		{
			*descrtype = STAB_V;
			break;
		}
		case 'x':
		{
			*descrtype = STAB_x;
			break;
		}
		case 'X':
		{
			*descrtype = STAB_X;
			break;
		}

		default:
			break;
	}

	return;
}

int
typename2size(char *typename)
{
	/*							*/
	/*	These should be the corresponding sizes in	*/
	/*	the simulated machine.	This is messy...	*/
	/*							*/

	if (typename == NULL)
	{
		return -1;
	}

	if (!strcmp(typename, "int"))
	{
		return 4;
	}
	if (!strcmp(typename, "char"))
	{
		return 1;
	}
	if (!strcmp(typename, "long int"))
	{
		return 4;
	}
	if (!strcmp(typename, "unsigned int"))
	{
		return 4;
	}
	if (!strcmp(typename, "long unsigned int"))
	{
		return 4;
	}
	if (!strcmp(typename, "long long int"))
	{
		return 8;
	}
	if (!strcmp(typename, "long long unsigned int"))
	{
		return 8;
	}
	if (!strcmp(typename, "short int"))
	{
		return 2;
	}
	if (!strcmp(typename, "short unsigned int"))
	{
		return 2;
	}
	if (!strcmp(typename, "signed char"))
	{
		return 1;
	}
	if (!strcmp(typename, "unsigned char"))
	{
		return 1;
	}
	if (!strcmp(typename, "float"))
	{
		return 4;
	}
	if (!strcmp(typename, "double"))
	{
		return 8;
	}
	if (!strcmp(typename, "long double"))
	{
		return 8;
	}

	return -1;
}

void
m_readstabs(State *S, char *filename)
{
	FILE	*fp;
	int	scope, nlines = 0;
	char	line[MAX_STABLINE_LEN], buf[MAX_STABSTR_LEN+1], *cleanbuf;
	Stab	stab;
	ulong	fnaddr = -1;
	int	scopedepth = 0;
	
	/*	+3 for the " *\0"	*/
	char	vdefn[MAX_STAB_TYPENAMELEN+1], *t = NULL;
	int	size;



	stab.name = (char *)mmalloc(MAX_STAB_VARNAMELEN, "(char *)stab.name in m_readstabs()");
	if (stab.name == NULL)
	{
		sfatal(S, "Malloc failed for (char *)stab.name in m_readstabs()");
	}

	fp = fopen(filename, "r");
	if (fp == NULL)
	{
		sfatal(S, "Could not open STABS file");
	}

	scope = SCOPE_GLOBAL;

	/*							*/
	/*		Skip all lines until the HdrSym		*/
	/*							*/
	for (; strstr(line, "HdrSym") == NULL ; nlines++)
	{
		fgets(line, sizeof(line), fp);
	}

	/*							*/
	/*	Note that string returned by fgetln() isn't	*/
	/*	NULL terminated.				*/
	/*							*/
	for (; fgets(line, sizeof(line), fp) != NULL; nlines++)
	{
		/*						*/
		/*	If last char in line is not newline, 	*/
		/*	munch till end.				*/
		/*						*/
		if ((strlen(line) > 0) && (line[strlen(line)-1] != '\n'))
		{		
			fscanf(fp, "%*s\n");
		}

		sscanf(line, "%d %s %d %d %llx %d%1024[^\n]",	/* 1024 = MAX_STABSTR_LEN */
			&stab.symnum, stab.ntype, &stab.nothr,
			&stab.ndesc, &stab.nvalue, &stab.nstrx, buf);
		cleanbuf = buf;
		while (cleanbuf[0] == ' ')
		{
			cleanbuf++;
		}

		if (!strcmp(stab.ntype, "SO"))
		{
			/*	New source file. Delete old type descriptor table	*/
			reset_types();
			continue;
		}
		else if (!strcmp(stab.ntype, "FUN"))
		{
			scope = SCOPE_FUNCTION;
			fnaddr = stab.nvalue;
			continue;
		}
		else if (!strcmp(stab.ntype, "LBRAC"))
		{
			scopedepth++;
			continue;
		}
		else if (!strcmp(stab.ntype, "RBRAC"))
		{
			if (scopedepth == 0)
			{
				sfatal(S,
				"Badly formatted stabs file: RBRAC when scope depth is zero!");
			}
			scopedepth--;
			if (scopedepth == 0)
			{
				scope = SCOPE_GLOBAL;
				fnaddr = -1;
			}
			continue;
		}


		parse_stabstr(S, cleanbuf, &stab.name, &stab.descrtype, &stab.type);
		if (	(stab.descrtype != STAB_STACKVAR) &&
			(stab.descrtype != STAB_G) &&
			(stab.descrtype != STAB_p) &&
			(stab.descrtype != STAB_P) &&
			(stab.descrtype != STAB_r) &&
			(stab.descrtype != STAB_S) &&
			(stab.descrtype != STAB_t) &&
			(stab.descrtype != STAB_V))
		{
			continue;
		}


		if (!strcmp(stab.ntype, "LSYM"))
		{
			if (stab.descrtype == STAB_t)
			{
				add_type(S, stab.type);
				continue;
			}

			/*								*/
			/*	Since we've excluded types via their 't' symbol descr	*/
			/*	this could only be a stack variable.			*/
			/*								*/
			if (scope != SCOPE_FUNCTION)
			{
				mprint(NULL, siminfo, "\tFound a stack variable outside a function on line %d\n", nlines);
				continue;
			}

			t = lookuptype(stab.type.a, stab.type.b);
			printf("Stack var \"%s\" in fn scope, typename \"%s\", fn start=0x%lx, frame offset=%llu,\tptr=%d\n",
				stab.name, t, fnaddr, stab.nvalue, stab.type.ispointer);

			if ((size = typename2size(t)) < 0)
			{
				/*	Not one of the basic types. Dont register it	*/
				continue;
			}
			snprintf(vdefn, MAX_STAB_TYPENAMELEN, "%s %s %s",
				t, (stab.type.ispointer ? "*" : ""), stab.name);
		
			m_addvaluetrace(S, vdefn, 0, size, 1, fnaddr, stab.nvalue, stab.type.ispointer);
		}
		else if (!strcmp(stab.ntype, "GSYM"))
		{
			if (scope != SCOPE_GLOBAL)
			{
				mprint(NULL, siminfo, "\tFound GSYM in non-global scope on line %d\n", nlines);
				continue;
			}

			printf("Global variable \"%s\" in global scope, typename \"%s\",\tptr=%d\n",
				stab.name, lookuptype(stab.type.a, stab.type.b), stab.type.ispointer);
		
			/*								*/
			/*	For global vars, must get address from Map file		*/
			/*								*/
		}
		else if (!strcmp(stab.ntype, "PSYM"))
		{
			if (scope != SCOPE_FUNCTION)
			{
				mprint(NULL, siminfo, "\tFound a stack variable outside a function on line %d\n", nlines);
				continue;
			}

			/*	Function parameter variable	*/
			if(stab.descrtype == STAB_p)
			{
				t = lookuptype(stab.type.a, stab.type.b);

				printf("Function param \"%s\" on stack, typename \"%s\", fn start=0x%lx, frame offset=%llu,\tptr=%d\n",
					stab.name, t, fnaddr, stab.nvalue, stab.type.ispointer);

				if ((size = typename2size(t)) < 0)
				{
					/*	Not one of the basic types. Dont register it	*/
					continue;
				}
				snprintf(vdefn, MAX_STAB_TYPENAMELEN, "%s %s %s",
					t, (stab.type.ispointer ? "*" : ""), stab.name);
				m_addvaluetrace(S, vdefn, 0, size, 1, fnaddr, stab.nvalue, stab.type.ispointer);
			}
			else
			{
				sfatal(S, "Unknown function parameter type");
			}
		}
		else if (!strcmp(stab.ntype, "RSYM"))
		{
			if (scope != SCOPE_FUNCTION)
			{
				mprint(NULL, siminfo, "\tFound a stack variable outside a function on line %d\n", nlines);
				continue;
			}

			/*	Register variable		*/
			if (stab.descrtype == STAB_P)
			{
				t = lookuptype(stab.type.a, stab.type.b);

				printf("Function param \"%s\" in reg, typename \"%s\", fn start=0x%lx, reg#=%llu,\tptr=%d\n",
					stab.name, t, fnaddr, stab.nvalue, stab.type.ispointer);

			}
			else if(stab.descrtype == STAB_r)
			{
				t = lookuptype(stab.type.a, stab.type.b);

				printf("Register var \"%s\" in fn scope, typename \"%s\", fn start=0x%lx, reg#=%llu,\tptr=%d\n",
					stab.name, t, fnaddr, stab.nvalue, stab.type.ispointer);
			}
			else
			{
				sfatal(S, "Unknown RSYM type");
			}

			if ((size = typename2size(t)) < 0)
			{
				/*	Not one of the basic types. Dont register it	*/
				continue;
			}
			snprintf(vdefn, MAX_STAB_TYPENAMELEN, "register %s %s %s",
				t, (stab.type.ispointer ? "*" : ""), stab.name);
			m_regtracer(S, vdefn, fnaddr, stab.nvalue, size, stab.type.ispointer);
		}
	}

	return;
}