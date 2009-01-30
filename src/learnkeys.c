#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <curses.h>
#include <term.h>

#define MAX_SEQUENCE 50
#define ESC 27
#define KEY_TIMEOUT 10

//FIXME: use ctrl-d for done
/* FIXME: ask for more control sequences to switch modes. For example for xterm
   we can split smkx into two seqs. This can be done by creating a table with these
   structures and then looping over that instead of the current kludge. */
//FIXME: test modifier-letter as well

typedef struct {
	const char *name;
	const char *identifier;
} Map;

static Map keynames[] = {
	{ "Insert", "insert" },
	{ "Home", "home" },
	{ "Page Up", "page_up" },
	{ "Delete", "delete" },
	{ "End", "end" },
	{ "Page Down", "page_down" },
	{ "Up", "up" },
	{ "Left", "left" },
	{ "Down", "down" },
	{ "Right", "right" },
	{ "Keypad Home", "kp_home" },
	{ "Keypad Up", "kp_up" },
	{ "Keypad Page Up", "kp_page_up" },
	{ "Keypad Left", "kp_left" },
	{ "Keypad Center", "kp_center" },
	{ "Keypad Right", "kp_right" },
	{ "Keypad End", "kp_end" },
	{ "Keypad Down", "kp_down" },
	{ "Keypad Page Down", "kp_page_down" },
	{ "Keypad Insert", "kp_insert" },
	{ "Keypad Delete", "kp_delete" },
};

static Map *functionkeys;

static Map modifiers[] = {
	{ "", "" },
	{ "Control ", "#c" },
	{ "Meta ", "#m" },
	{ "Shift ", "#s" },
	{ "Control+Meta ", "#cm" },
	{ "Control+Shift ", "#cs" },
	{ "Meta+Shift ", "#ms" },
	{ "Control+Meta+Shift ", "#cms" },
};

#define SIZEOF(_x) ((sizeof(_x)/sizeof(_x[0])))

typedef struct Sequence {
	struct Sequence *next;
	char seq[MAX_SEQUENCE * 4 + 1];
	Map *modifiers;
	Map *keynames;
	bool duplicate;
} Sequence;

static struct termios saved;
static fd_set inset;
static bool xterm_restore;


/** Alert the user of a fatal error and quit.
    @param fmt The format string for the message. See fprintf(3) for details.
    @param ... The arguments for printing.
*/
void fatal(const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	exit(EXIT_FAILURE);
}

static void restore_terminal(void) {
	char *rmkx = tigetstr("rmkx");
	if (rmkx != NULL && rmkx != (void *) -1)
		printf("%s", rmkx);
	if (xterm_restore)
		printf("\x1b[?1036r");
	tcsetattr(STDOUT_FILENO, TCSADRAIN, &saved);
}

static void init_terminal(void) {
	char *env;
	struct termios new_params;
	if (!isatty(STDOUT_FILENO))
		fatal("Stdout is not a terminal\n");

	setupterm((char *)0, 1, (int *)0);

	if (tcgetattr(STDOUT_FILENO, &saved) < 0)
		fatal("Could not retrieve terminal settings: %m\n");

	new_params = saved;
	new_params.c_iflag &= ~(IXON | IXOFF);
	new_params.c_iflag |= INLCR;
	new_params.c_lflag &= ~(ISIG | ICANON | ECHO);
	new_params.c_cc[VMIN] = 1;

	if (tcsetattr(STDOUT_FILENO, TCSADRAIN, &new_params) < 0)
		fatal("Could not change terminal settings: %m\n");

	atexit(restore_terminal);
	FD_ZERO(&inset);
	FD_SET(STDIN_FILENO, &inset);
	env = getenv("TERM");
	if (strncmp("xterm", env, 5) == 0) {
		printf("\x1b[?1036s\x1b[?1036h");
		xterm_restore = true;
	}

}

static int safe_read_char(void) {
	char c;
	while (1) {
		ssize_t retval = read(STDIN_FILENO, &c, 1);
		if (retval < 0 && errno == EINTR)
			continue;
		else if (retval >= 1)
			return c;

		return -1;
	}
}

static int get_keychar(int msec) {
	int retval;
	fd_set _inset;
	struct timeval timeout;


	while (1) {
		_inset = inset;
		if (msec > 0) {
			timeout.tv_sec = msec / 1000;
			timeout.tv_usec = (msec % 1000) * 1000;
		}

		retval = select(STDIN_FILENO + 1, &_inset, NULL, NULL, msec > 0 ? &timeout : NULL);

		if (retval < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		} else if (retval == 0) {
			return -1;
		} else {
			return safe_read_char();
		}
	}
}

Sequence *get_sequence(void) {
	Sequence *retval;
	char seq[MAX_SEQUENCE];
	int c, idx = 0;

	do {
		c = get_keychar(-1);
		if (c == ESC) {
			int i, dest = 0;

			seq[idx++] = ESC;

			while ((c = get_keychar(KEY_TIMEOUT)) != -1 && idx < MAX_SEQUENCE)
				seq[idx++] = c;

			if (idx == MAX_SEQUENCE) {
				printf("sequence too long");
				return NULL;
			} else if (idx < 2) {
				printf("sequence too short");
				return NULL;
			}

			if ((retval = malloc(sizeof(Sequence))) == NULL)
				fatal("Out of memory\n");

			for (i = 0; i < idx; i++) {
				if (isprint(seq[i])) {
					retval->seq[dest++] = seq[i];
				} else if (seq[i] == 27) {
					retval->seq[dest++] = '\\';
					retval->seq[dest++] = 'e';
				} else {
					sprintf(retval->seq + dest, "\\%03o", seq[i]);
					dest += 4;
				}
			}
			retval->seq[dest] = 0;
			retval->duplicate = FALSE;
			return retval;
		} else if (c == 3) {
			printf("\n");
			exit(EXIT_SUCCESS);
		} else if (c == 18) {
			return (void *) -1;
		} else if (c >= 0) {
			while (get_keychar(KEY_TIMEOUT) >= 0) {}
			printf("(no escape sequence)");
			return NULL;
		}
	} while (c < 0);
	return NULL;
}

static Sequence *head = NULL;
static FILE *output;

static int getkeys(Map *keys, int max, int mod) {
	Sequence *current;
	int i;
	for (i = 0; i < max; i++) {
		Sequence *new_seq;
		printf("%s%s ", modifiers[mod].name, keys[i].name);
		fflush(stdout);
		new_seq = get_sequence();
		if (new_seq == NULL) {
			printf("\n");
			continue;
		} else if (new_seq == (void *) -1) {
			return -1;
		}
		printf("%s ", new_seq->seq);
		current = head;
		while (current != NULL) {
			if (strcmp(current->seq, new_seq->seq) == 0 && !current->duplicate) {
				printf("(duplicate for %s%s)", current->modifiers->name, current->keynames->name);
				new_seq->duplicate = TRUE;
				break;
			} else {
				current = current->next;
			}
		}
		new_seq->modifiers = modifiers + mod;
		new_seq->keynames = keys + i;
		new_seq->next = head;
		head = new_seq;
		printf("\n");
	}
	return 0;
}

static void printmap(Map *keys, int max, int mod) {
	Sequence *current, *last;
	int i;

	for (i = 0; i < max; i++) {
		for (current = head; current != NULL; current = current->next) {
			if (current->modifiers == modifiers + mod && current->keynames == keys + i)
				break;
		}
		if (current == NULL) {
			fprintf(output, "# %s%s (no sequence)\n", keys[i].identifier, modifiers[mod].identifier);
		} else {
			if (current->duplicate) {
				last = current;
				// Find first key for which this is a duplicate
				while (last != NULL) {
					if (strcmp(current->seq, last->seq) == 0 && !last->duplicate)
						break;
					last = last->next;
				}
				fprintf(output, "%s%s = %s%s\n", keys[i].identifier, modifiers[mod].identifier,
					last->keynames->identifier, last->modifiers->identifier);
			} else {
				fprintf(output, "%s%s = \"%s\"\n", keys[i].identifier, modifiers[mod].identifier, current->seq);
			}
		}
	}
}

int main(int argc, char *argv[]) {
	size_t i;
	int maxfkeys = -1;
	char *smkx = NULL, *rmkx = NULL;
	const char *term = getenv("TERM");

	(void) argc;
	(void) argv;

	if (term == NULL)
		fatal("No terminal type has been set in the TERM environment variable\n");

	if ((output = fopen(term, "w")) == NULL)
		fatal("Can't open output file '%s': %m\n", term);

	printf("libckey key learning program\n");
	printf("Learning keys for terminal %s. Please press the requested key or enter\n", term);
	printf("WARNING: Be carefull when pressing combinations as they may be bound to actions\nyou don't want to execute! For best results don't run this in a window manager.\n");
	do {
		printf("How many function keys does your keyboard have? ");
		scanf("%d", &maxfkeys);
	} while (maxfkeys < 0);
	if (maxfkeys > 0) {
		functionkeys = malloc(maxfkeys * sizeof(Map));
		for (i = 0; (int) i < maxfkeys; i++) {
			char buffer[1024];
			snprintf(buffer, 1024, "F%d", i + 1);
			functionkeys[i].name = strdup(buffer);
			snprintf(buffer, 1024, "f%d", i + 1);
			functionkeys[i].identifier = strdup(buffer);
		}
	}


	init_terminal();

	rmkx = tigetstr("rmkx");
	if (rmkx != NULL && rmkx != (void *) -1)
		printf("%s", rmkx);

	while (1) {
		Sequence *current, *last, *before_insert;
		int c;
		for (i = 0; i < SIZEOF(modifiers); i++) {
			before_insert = head;
			if (getkeys(keynames, SIZEOF(keynames), i) < 0) {
				printf("\nRestarting...\n");
				goto skip;
			}
			if (getkeys(functionkeys, maxfkeys, i) < 0) {
				printf("\nRestarting...\n");
				goto skip;
			}

			do {
				printf("Are you satisfied with the above keys? ");
				fflush(stdout);
				c = safe_read_char();
				printf("\n");
			} while (c != 'y' && c != 'Y' && c != 'n' && c != 'N');

			if (c == 'n' || c == 'N') {
		skip:
				while (head != before_insert) {
					current = head;
					head = current->next;
					free(current);
				}
				i--;
			}
		}

		fprintf(output, "%s {\n", smkx == NULL ? "nokx" : "kx");
		if (smkx != NULL)
			fprintf(output, "%%enter = smkx\n%%leave = rmkx\n");
		else if (rmkx != NULL)
			fprintf(output, "%%enter = rmkx\n");

		for (i = 0; i < SIZEOF(modifiers); i++) {
			printmap(keynames, SIZEOF(keynames), i);
			printmap(functionkeys, maxfkeys, i);
		}
		for (current = head, last = NULL; current != NULL; last = current, current = current->next)
			free(last);

		fprintf(output, "}\n");
		head = NULL;

		if (smkx == NULL) {
			smkx = tigetstr("smkx");
			if (smkx == NULL || smkx == (void *) -1)
				break;
			printf("%sSwitching to keypad transmit\n", smkx);
		} else {
			break;
		}
	}
	fflush(output);
	fclose(output);
	return 0;
}
