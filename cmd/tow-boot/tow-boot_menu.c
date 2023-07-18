// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2021 Samuel Dionne-Riel <samuel@dionne-riel.com>
 */

#include <PDCurses/curses.h>

#include <command.h>
#include <common.h>
#include <linux/compat.h>
#include <serial.h>
#include <splash.h>
#include <vsprintf.h>

#define str(s) #s
#define xstr(s) str(s)
#define MAX_LEN 30

#define KEY_CTRL_C 0x03

typedef struct menuentry {
	int number;
	char *label;
	char *description;
	char *command;
	bool separator;

	struct menuentry *previous;
	struct menuentry *next;
} menuentry;

typedef struct menustate {
	WINDOW *window;
	WINDOW *menu;
	WINDOW *help;

	menuentry *first_entry;
	menuentry *last_entry;
} menustate;

// The menu will always be stored in here.
static menustate *current_menu = NULL;

static void draw_item(menustate *state, int i, bool selected)
{
	char label[MAX_LEN+3];
	menuentry *iter = state->first_entry;

	if (selected) {
		wattron(state->menu, A_STANDOUT);
	}

	while (iter->number != i) {
		if (!iter->next) { return; }
		iter = iter->next;
	}

	// A separator is just an empty line.
	if (iter->separator) {
		sprintf(label, "");
	}
	else {
		sprintf(label, " %-" xstr(MAX_LEN) "s ", iter->label);
	}
	mvwprintw(state->menu, iter->number-1, 0, "%s", label);

	if (selected) {
		wattroff(state->menu, A_STANDOUT);
		sprintf(label, " %-" xstr(MAX_LEN) "s ", iter->description);
		mvwprintw(current_menu->help, 0, 0, "%s", label);
	}
}

static void printmenu(menustate *state)
{
	menuentry *iter = state->first_entry;

	// Draw everything once
	while (iter) {
		draw_item(state, iter->number, FALSE);
		iter = iter->next;
	}

	// Select the first item.
	draw_item(state, 1, TRUE);
}

static menuentry *init_entry(menuentry *previous)
{
	menuentry *entry = calloc(1, sizeof(menuentry));

	if (previous) {
		entry->number = previous->number + 1;
		previous->next = entry;
	}
	else {
		entry->number = 1;
	}

	entry->previous = previous;
	entry->next = NULL;
	entry->separator = false;

	return entry;
}

static int menu_num_entries(menustate *state)
{
	if (state->last_entry) {
		return state->last_entry->number;
	}
	else {
		return 0;
	}
}

menuentry * menu_add_entry(menustate *state, char *label, char *description, char *command)
{
	menuentry *entry = init_entry(state->last_entry);
	if (!state->first_entry) {
		state->first_entry = entry;
	}
	state->last_entry = entry;

	entry->label = label;
	entry->description = description;
	entry->command = command;

	return entry;
}

static void menu_delete(menustate *state)
{
	menuentry *iter = state->first_entry;
	menuentry *next = NULL;
	while (iter) {
		next = iter->next;
		free(iter);
		iter = next;
	}
	free(state);
}

static menuentry *menu_get_item(menustate *state, int i) {
	menuentry *iter = state->first_entry;

	while (iter) {
		if (iter->number == i) {
			return iter;
		}
		iter = iter->next;
	}

	return NULL;
}

static int do_new(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (current_menu) {
		fprintf(stderr, "Clearing old menu...\n");
		menu_delete(current_menu);
	}

	fprintf(stderr, "Creating menu...\n");
	current_menu = calloc(1, sizeof(menustate));
	
	return CMD_RET_SUCCESS;
}

static int do_show(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (!current_menu) {
		fprintf(stderr, "error: No menu was built... Aborting.\n");
		return CMD_RET_FAILURE;
	}
	if (!current_menu->first_entry) {
		fprintf(stderr, "error: Menu is empty... Aborting.\n");
		return CMD_RET_FAILURE;
	}

	int ch = 0;
	int curr = 1;
	int menu_width = MAX_LEN;
	int menu_height = 0;
	bool do_run = false;
	bool quit = false;
	// Will be used to store the command to run at the end.
	char *command = NULL;
	int ret = CMD_RET_SUCCESS;

	run_command("cls", 0);

	initscr();

	menu_height = menu_num_entries(current_menu);

	// Creates the "canvas" for the menu, centered.
	current_menu->window = newwin(
		menu_height + 2 + 2, menu_width + 4,
		0, 0
	);

	// Line for the help text
	current_menu->help = derwin(
		current_menu->window,
		1, menu_width + 2,
		menu_height + 2, 1
	);
	syncok(current_menu->help, TRUE);

	// "Viewport" for the menu listing
	current_menu->menu = derwin(
		current_menu->window,
		menu_height, menu_width + 2,
		1, 1
	);
	syncok(current_menu->menu, TRUE);

	mvwin(
		current_menu->window,
		(LINES-menu_height-2)/2,
		(COLS-menu_width)/2
	);

	// Draw a box around the window
	box(current_menu->window, 0, 0);

	// Draw a separating line for the help
	mvwaddch(current_menu->window, menu_height+1, 0, ACS_LTEE);
	whline(  current_menu->window, 0, menu_width+2);
	mvwaddch(current_menu->window, menu_height+1, menu_width+3, ACS_RTEE);

	// Silence inputs
	noecho();
	curs_set(0);

	// Enables key-based navigation
	keypad(current_menu->window, TRUE);

	printmenu(current_menu);

	wrefresh(stdscr);

	while(!quit){

#if defined(CONFIG_SPLASH_SCREEN) && defined(CONFIG_CMD_BMP)
		// Always re-draw the splash at every menu refresh.
		splash_display();
#endif

		ch = wgetch(current_menu->window);

		switch(ch) {
			// "Quitting" the menu this way is a misfeature here.
			// It should be resilient to all weird input, and only
			// exit to shell in a controlled manner.
			// This is because a user "spamming delete" to get in the BIOS menu
			// is a supported user story with Tow-Boot.
			//
			// >  - As a user
			// >  - I want to spam a keyboard key
			// >  - To get to the boot options / configuration menu
			//
#ifdef CONFIG_TOW_BOOT_MENU_CTRL_C_EXITS
			case KEY_CTRL_C:
				quit = true;
				break;
#endif
			case KEY_UP:
				draw_item(current_menu, curr, FALSE);
				do {
					curr--;
					curr = (curr<1) ? menu_num_entries(current_menu) : curr;
				} while (menu_get_item(current_menu, curr)->separator);
				draw_item(current_menu, curr, TRUE);
				break;
			case KEY_DOWN:
				draw_item(current_menu, curr, FALSE);
				do {
					curr++;
					curr = (curr > menu_num_entries(current_menu)) ? 1 : curr;
				} while (menu_get_item(current_menu, curr)->separator);
				draw_item(current_menu, curr, TRUE);
				break;
			case '\n':
				if (!menu_get_item(current_menu, curr)->separator) {
					do_run = true;
					quit = true;
				}
				break;
			default:
#ifdef DEBUG
				serial_printf("Unhandled char: 0x%x; // '%c'\n", (unsigned int)ch, ch);
#endif
				break;
		}
	}

	if (do_run) {
		// Save the command, we're deleting the menu
		command = strdup(menu_get_item(current_menu, curr)->command);
	}

	// Cleanup
	delwin(current_menu->menu);
	delwin(current_menu->window);
	wrefresh(stdscr);
	endwin();

	menu_delete(current_menu);
	current_menu = NULL;

#if defined(CONFIG_SPLASH_SCREEN) && defined(CONFIG_CMD_BMP)
	// Do a final re-draw, just before we run the command.
	splash_display();
#endif

	if (do_run) {
		// Run the command
		ret = run_command(command, 0);
		free(command);
	}

	return ret;
}

static int do_separator(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	menuentry *entry = NULL;;
	if (!current_menu) {
		current_menu = calloc(1, sizeof(menustate));
	}

	entry = menu_add_entry(current_menu, "", "", "");
	entry->separator = true;

	return CMD_RET_SUCCESS;
}

static int do_add(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	char *label = NULL;
	char *description = NULL;
	char *command = NULL;

	if (argc < 4) {
		fprintf(stderr, "error: Not enough parameters...\n");
		return CMD_RET_USAGE;
	}

	if (argc > 4) {
		fprintf(stderr, "error: Too many parameters...\n");
		return CMD_RET_USAGE;
	}

	if (!current_menu) {
		current_menu = calloc(1, sizeof(menustate));
	}

	label = strdup(argv[1]);
	description = strdup(argv[2]);
	command = strdup(argv[3]);

	(void)menu_add_entry(current_menu, label, description, command);

	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_tb_menu_sub[] = {
	U_BOOT_CMD_MKENT(new, 1, 0, do_new, "", ""),
	U_BOOT_CMD_MKENT(show, 1, 0, do_show, "", ""),
	U_BOOT_CMD_MKENT(add, CONFIG_SYS_MAXARGS, 0, do_add, "", ""),
	U_BOOT_CMD_MKENT(separator, 1, 0, do_separator, "", ""),
};

static int do_tb_menu(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct cmd_tbl *cp;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* drop sub-command argument */
	argc--;
	argv++;

	cp = find_cmd_tbl(argv[0], cmd_tb_menu_sub, ARRAY_SIZE(cmd_tb_menu_sub));

	if (cp)
		return cp->cmd(cmdtp, flag, argc, argv);

	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	tb_menu,
	CONFIG_SYS_MAXARGS,
	0,
	do_tb_menu,
	"Tow-Boot curses-based menu interface.",
	"Menus are ephemeral. They need to be rebuilt every time they are shown.\n"
	"\n"
	"tb_menu new                                  - Starts writing a new menu, dropping the current one. Optional.\n"
	"tb_menu show                                 - Shows the current menu, dropped on exit.\n"
	"tb_menu add <label> <description> <command>  - Add entry\n"
	"tb_menu separator                            - Add a separator\n"
)
