/*
 * Copyright 2014 Dirk Eibach <eibach@gdsys.de>
 *           2022 Alex Kiselev <a.kiselev@volz-servos.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "defines.h"
#include "c2tool.h"
#include "log.h"

static int cmd_size;

extern struct cmd __start___cmd;
extern struct cmd __stop___cmd;

#define for_each_cmd(_cmd)					\
	for (_cmd = &__start___cmd; _cmd < &__stop___cmd;		\
	     _cmd = (const struct cmd *)((char *)_cmd + cmd_size))


static void __usage_cmd(const struct cmd *cmd, char *indent, bool full)
{
	const char *start, *lend, *end;

	printf("%s", indent);

	printf("%s", cmd->name);

	if (cmd->args) {
		/* print line by line */
		start = cmd->args;
		end = strchr(start, '\0');
		printf(" ");
		do {
			lend = strchr(start, '\n');
			if (!lend)
				lend = end;
			if (start != cmd->args) {
				printf("\t");
				printf("%s ", cmd->name);
			}
			printf("%.*s\n", (int)(lend - start), start);
			start = lend + 1;
		} while (end != lend);
	} else
		printf("\n");

	if (!full || !cmd->help)
		return;

	/* hack */
	if (strlen(indent))
		indent = "\t\t";
	else
		printf("\n");

	/* print line by line */
	start = cmd->help;
	end = strchr(start, '\0');
	do {
		lend = strchr(start, '\n');
		if (!lend)
			lend = end;
		printf("%s", indent);
		printf("%.*s\n", (int)(lend - start), start);
		start = lend + 1;
	} while (end != lend);

	printf("\n");
}

static const char *argv0;

static void usage(int argc, char **argv)
{
	const struct cmd *cmd;
	bool full = argc >= 0;
	const char *cmd_filt = NULL;

	if (argc > 0)
		cmd_filt = argv[0];

	printf("Usage:\t%s <gpio c2d> <gpio c2ck> command\n", argv0);
	printf("Commands:\n");
	for_each_cmd(cmd) {
		if (!cmd->handler || cmd->hidden)
			continue;
		if (cmd_filt && strcmp(cmd->name, cmd_filt))
			continue;
		__usage_cmd(cmd, "\t", full);
	}
}

static void usage_cmd(const struct cmd *cmd)
{
	printf("Usage:\t%s [options] ", argv0);
	__usage_cmd(cmd, "", true);
}

static int __handle_cmd(struct c2tool_state *state, int argc, char **argv,
			const struct cmd **cmdout)
{
	const struct cmd *cmd, *match = NULL;
	const char *command;

	if (argc <= 0)
		return 1;

	if (argc > 0) {
		command = *argv;

		for_each_cmd(cmd) {
			if (!cmd->handler)
				continue;
			if (strcmp(cmd->name, command))
				continue;
			if (argc > 1 && !cmd->args)
				continue;
			match = cmd;
			break;
		}

		if (match) {
			argc--;
			argv++;
		}
	}

	if (match)
		cmd = match;
	else
		return 1;

	if (cmdout)
		*cmdout = cmd;

	return cmd->handler(state, argc, argv);
}

int handle_cmd(struct c2tool_state *state, int argc, char **argv)
{
	return __handle_cmd(state, argc, argv, NULL);
}

HIDDEN(dummy1, NULL, NULL);
HIDDEN(dummy2, NULL, NULL);

int handle_version(struct c2tool_state *state, int argc, char **argv)
{
	printf("c2tool version %s\n", c2tool_version);

	return 0;
}

COMMAND(version, "", handle_version, "Show SW version.");

int main(int argc, char **argv)
{
	int err;
	const struct cmd *cmd = NULL;
	struct c2_device_info info;
	struct c2tool_state state;

	/* calculate command size including padding */
	cmd_size = abs((long)&__cmd_dummy1 - (long)&__cmd_dummy2);
	/* strip off self */
	argc--;
	argv0 = *argv++;

	if (argc == 0) {
		usage(0, NULL);
		return 1;
	}

	LOG_SetLevel(LOG_LEVEL_WARNING);

	/* Check for version command first to allow it without hardware */
	if (argc > 0) {
		const struct cmd *cmd;
		const char *command = argv[0];

		for_each_cmd(cmd) {
			if (strcmp(cmd->name, command) == 0 && 
			    strcmp(command, "version") == 0) {
				return cmd->handler(NULL, argc, argv);
			}
		}
	}

	if (c2_init() < 0)
		return 1;

	if (c2_halt(&state.c2if) < 0)
		return 1;

	if (c2_get_device_info(&state.c2if, &info) < 0)
		return 1;

	if (c2family_find(info.device_id, &state.family)) {
		fprintf(stderr, "device family 0x%02x not supported yet\n", info.device_id);
		return 1;
	}

	err = __handle_cmd(&state, argc, argv, &cmd);

	if (err == 1) {
		if (cmd)
			usage_cmd(cmd);
		else
			usage(0, NULL);
	} else if (err < 0)
		fprintf(stderr, "command failed: %s (%d)\n", strerror(-err), err);

	c2_terminate();

	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
