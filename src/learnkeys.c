#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <curses.h>

#define MAX_SEQUENCE 50
#define ESC 27

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
} Sequence;

static struct termios saved;
static fd_set inset;


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
	tcsetattr(STDOUT_FILENO, TCSADRAIN, &saved);
}

static void init_terminal(void) {
	struct termios new_params;
	if (!isatty(STDOUT_FILENO))
		fatal("Stdout is not a terminal\n");

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

			while ((c = get_keychar(100)) != -1 && idx < MAX_SEQUENCE)
				seq[idx++] = c;

			if (idx == MAX_SEQUENCE) {
				printf("sequence too long");
				return NULL;
			} else if (idx < 3) {
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
			return retval;
		} else if (c == 3) {
			printf("\n");
			exit(EXIT_SUCCESS);
		} else if (c >= 0) {
			while (get_keychar(100) >= 0) {}
			printf(" (no escape sequence)");
			return NULL;
		}
	} while (c < 0);
	return NULL;
}

int main(int argc, char *argv[]) {
	size_t i, j;
	Sequence *head = NULL;
	const char *term = getenv("TERM");

	if (term == NULL)
		fatal("No terminal type has been set in the TERM environment variable\n");

	init_terminal();

	printf("libckey key learning program\n");
	printf("Learning keys for terminal %s. Please press the requested key or enter\n", term);
//FIXME don't ask for obvious problem keys like shift pgup/pgdn and ctrl-alt delete
//FIXME once for keypad TX, once without
	for (i = 0; i < SIZEOF(modifiers); i++) {
		for (j = 0; j < SIZEOF(keynames); j++) {
			Sequence *new_seq, *current;
			printf("%s%s", modifiers[i].name, keynames[j].name);
			fflush(stdout);
			new_seq = get_sequence();
			if (new_seq == NULL) {
				printf("\n");
				continue;
			}
			printf(" %s ", new_seq->seq);
			current = head;
			while (current) {
				if (strcmp(current->seq, new_seq->seq) == 0) {
					printf("(duplicate for %s%s)\n", current->modifiers->name, current->keynames->name);
					free(new_seq);
					new_seq = NULL;
					current = NULL;
				} else {
					current = current->next;
				}
			}
			if (new_seq != NULL) {
				new_seq->modifiers = modifiers + i;
				new_seq->keynames = keynames + j;
				new_seq->next = head;
				head = new_seq;
				printf("\n");
			}
		}
	}
	return 0;
}