/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2013-2018 K. Lange
 */
#include <kernel/system.h>
#include <kernel/fs.h>
#include <kernel/pipe.h>
#include <kernel/logging.h>
#include <kernel/printf.h>
#include <kernel/ringbuffer.h>
#include <toaru/hashmap.h>

#include <sys/ioctl.h>
#include <sys/termios.h>

#define TTY_BUFFER_SIZE 4096
//4096

typedef struct pty {
	/* the PTY number */
	int            name;

	/* Master and slave endpoints */
	fs_node_t *    master;
	fs_node_t *    slave;

	/* term io "window size" struct (width/height) */
	struct winsize size;

	/* termios data structure */
	struct termios tios;

	/* directional pipes */
	ring_buffer_t * in;
	ring_buffer_t * out;

	char * canon_buffer;
	size_t canon_bufsize;
	size_t canon_buflen;

	pid_t ct_proc; /* Controlling process (shell) */
	pid_t fg_proc; /* Foreground process (might also be shell) */

} pty_t;

static int _pty_counter = 0;
static hashmap_t * _pty_index = NULL;
static fs_node_t * _pty_dir = NULL;

#define IN(character)   ring_buffer_write(pty->in, 1, (uint8_t *)&(character))
#define OUT(character)  ring_buffer_write(pty->out, 1, (uint8_t *)&(character))

static void dump_input_buffer(pty_t * pty) {
	char * c = pty->canon_buffer;
	while (pty->canon_buflen > 0) {
		IN(*c);
		pty->canon_buflen--;
		c++;
	}
}

static void clear_input_buffer(pty_t * pty) {
	pty->canon_buflen = 0;
	pty->canon_buffer[0] = '\0';
}

static void output_process_slave(pty_t * pty, uint8_t c) {
	if (c == '\n' && (pty->tios.c_oflag & ONLCR)) {
		c = '\n';
		OUT(c);
		c = '\r';
		OUT(c);
		return;
	}

	if (c == '\r' && (pty->tios.c_oflag & ONLRET)) {
		return;
	}

	if (c >= 'a' && c <= 'z' && (pty->tios.c_oflag & OLCUC)) {
		c = c + 'a' - 'A';
		OUT(c);
		return;
	}

	OUT(c);
}

static void output_process(pty_t * pty, uint8_t c) {
	if (ring_buffer_available(pty->out) < 2) return; /* uh oh */
	output_process_slave(pty, c);
}

static void input_process(pty_t * pty, uint8_t c) {
	if (pty->tios.c_lflag & ISIG) {
		if (c == pty->tios.c_cc[VINTR]) {
			if (pty->tios.c_lflag & ECHO) {
				output_process(pty, '^');
				output_process(pty, '@' + c);
				output_process(pty, '\n');
			}
			clear_input_buffer(pty);
			if (pty->fg_proc) {
				send_signal(pty->fg_proc, SIGINT, 1);
			}
			return;
		}
		if (c == pty->tios.c_cc[VQUIT]) {
			if (pty->tios.c_lflag & ECHO) {
				output_process(pty, '^');
				output_process(pty, '@' + c);
				output_process(pty, '\n');
			}
			clear_input_buffer(pty);
			if (pty->fg_proc) {
				send_signal(pty->fg_proc, SIGQUIT, 1);
			}
			return;
		}
		/* VSUSP */
	}
#if 0
	if (pty->tios.c_lflag & IXON ) {
		/* VSTOP, VSTART */
	}
#endif
	if (pty->tios.c_lflag & ICANON) {
		if (c == pty->tios.c_cc[VKILL]) {
			while (pty->canon_buflen > 0) {
				pty->canon_buflen--;
				pty->canon_buffer[pty->canon_buflen] = '\0';
				if ((pty->tios.c_lflag & ECHO) && (pty->tios.c_lflag & ECHOK)) {
					output_process(pty, '\010');
					output_process(pty, ' ');
					output_process(pty, '\010');
				}
			}
			return;
		}
		if (c == pty->tios.c_cc[VERASE]) {
			/* Backspace */
			if (pty->canon_buflen > 0) {
				pty->canon_buflen--;
				pty->canon_buffer[pty->canon_buflen] = '\0';
				if ((pty->tios.c_lflag & ECHO) && (pty->tios.c_lflag & ECHOE)) {
					output_process(pty, '\010');
					output_process(pty, ' ');
					output_process(pty, '\010');
				}
			}
			return;
		}
		if (c == pty->tios.c_cc[VEOF]) {
			if (pty->canon_buflen) {
				dump_input_buffer(pty);
			} else {
				ring_buffer_interrupt(pty->in);
			}
			return;
		}

		/* ISTRIP: Strip eighth bit */
		if (pty->tios.c_iflag & ISTRIP) {
			c &= 0x7F;
		}

		/* IGNCR: Ignore carriage return. */
		if ((pty->tios.c_iflag & IGNCR) && c == '\r') {
			return;
		}

		/* INLCR: Translate NL to CR. */
		if ((pty->tios.c_iflag & INLCR) && c == '\n') {
			c = '\r';
		}

		/* ICRNL: Convert carriage return. */
		if ((pty->tios.c_iflag & ICRNL) && c == '\r') {
			c = '\n';
		}

		if (pty->canon_buflen < pty->canon_bufsize) {
			pty->canon_buffer[pty->canon_buflen] = c;
			pty->canon_buflen++;
		}
		if (pty->tios.c_lflag & ECHO) {
			output_process(pty, c);
		}
		if (c == '\n') {
			if (!(pty->tios.c_lflag & ECHO) && (pty->tios.c_lflag & ECHONL)) {
				output_process(pty, c);
			}
			pty->canon_buffer[pty->canon_buflen-1] = c;
			dump_input_buffer(pty);
			return;
		}
		return;
	} else if (pty->tios.c_lflag & ECHO) {
		output_process(pty, c);
	}
	IN(c);
}

int pty_ioctl(pty_t * pty, int request, void * argp) {
	switch (request) {
		case IOCTLDTYPE:
			/*
			 * This is a special toaru-specific call to get a simple
			 * integer that describes the kind of device this is.
			 * It's more specific than just "character device" or "file",
			 * but for here we just need to say we're a TTY.
			 */
			return IOCTL_DTYPE_TTY;
		case IOCTLTTYNAME:
			if (!argp) return -1;
			validate(argp);
			((char*)argp)[0] = '\0';
			sprintf((char*)argp, "/dev/pts/%d", pty->name);
			return 0;
		case TIOCSWINSZ:
			if (!argp) return -1;
			validate(argp);
			memcpy(&pty->size, argp, sizeof(struct winsize));
			/* TODO send sigwinch to fg_prog */
			return 0;
		case TIOCGWINSZ:
			if (!argp) return -1;
			validate(argp);
			memcpy(argp, &pty->size, sizeof(struct winsize));
			return 0;
		case TCGETS:
			if (!argp) return -1;
			validate(argp);
			memcpy(argp, &pty->tios, sizeof(struct termios));
			return 0;
		case TIOCSPGRP:
			if (!argp) return -1;
			validate(argp);
			pty->fg_proc = *(pid_t *)argp;
			debug_print(NOTICE, "Setting PTY group to %d", pty->fg_proc);
			return 0;
		case TIOCGPGRP:
			if (!argp) return -1;
			validate(argp);
			*(pid_t *)argp = pty->fg_proc;
			return 0;
		case TCSETS:
		case TCSETSW:
		case TCSETSF:
			if (!argp) return -1;
			validate(argp);
			if (!(((struct termios *)argp)->c_lflag & ICANON) && (pty->tios.c_lflag & ICANON)) {
				/* Switch out of canonical mode, the dump the input buffer */
				dump_input_buffer(pty);
			}
			memcpy(&pty->tios, argp, sizeof(struct termios));
			return 0;
		default:
			return -EINVAL;
	}
}

uint32_t  read_pty_master(fs_node_t * node, uint32_t offset, uint32_t size, uint8_t *buffer) {
	pty_t * pty = (pty_t *)node->device;

	/* Standard pipe read */
	return ring_buffer_read(pty->out, size, buffer);
}
uint32_t write_pty_master(fs_node_t * node, uint32_t offset, uint32_t size, uint8_t *buffer) {
	pty_t * pty = (pty_t *)node->device;

	size_t l = 0;
	for (uint8_t * c = buffer; l < size; ++c, ++l) {
		input_process(pty, *c);
	}

	return l;
}
void      open_pty_master(fs_node_t * node, unsigned int flags) {
	return;
}
void     close_pty_master(fs_node_t * node) {
	return;
}

uint32_t  read_pty_slave(fs_node_t * node, uint32_t offset, uint32_t size, uint8_t *buffer) {
	pty_t * pty = (pty_t *)node->device;

	if (pty->tios.c_lflag & ICANON) {
		return ring_buffer_read(pty->in, size, buffer);
	} else {
		if (pty->tios.c_cc[VMIN] == 0) {
			return ring_buffer_read(pty->in, MIN(size, ring_buffer_unread(pty->in)), buffer);
		} else {
			return ring_buffer_read(pty->in, MIN(pty->tios.c_cc[VMIN], size), buffer);
		}
	}
}

uint32_t write_pty_slave(fs_node_t * node, uint32_t offset, uint32_t size, uint8_t *buffer) {
	pty_t * pty = (pty_t *)node->device;

	size_t l = 0;
	for (uint8_t * c = buffer; l < size; ++c, ++l) {
		output_process_slave(pty, *c);
	}

	return l;
}
void      open_pty_slave(fs_node_t * node, unsigned int flags) {
	return;
}
void     close_pty_slave(fs_node_t * node) {
	pty_t * pty = (pty_t *)node->device;

	hashmap_remove(_pty_index, (void*)pty->name);

	return;
}

/*
 * These are separate functions just in case I ever feel the need to do
 * things differently in the slave or master.
 */
int ioctl_pty_master(fs_node_t * node, int request, void * argp) {
	pty_t * pty = (pty_t *)node->device;
	return pty_ioctl(pty, request, argp);
}

int ioctl_pty_slave(fs_node_t * node, int request, void * argp) {
	pty_t * pty = (pty_t *)node->device;
	return pty_ioctl(pty, request, argp);
}

int pty_available_input(fs_node_t * node) {
	pty_t * pty = (pty_t *)node->device;
	return ring_buffer_unread(pty->in);
}

int pty_available_output(fs_node_t * node) {
	pty_t * pty = (pty_t *)node->device;
	return ring_buffer_unread(pty->out);
}

static int check_pty_master(fs_node_t * node) {
	pty_t * pty = (pty_t *)node->device;
	if (ring_buffer_unread(pty->out) > 0) {
		return 0;
	}
	return 1;
}

static int check_pty_slave(fs_node_t * node) {
	pty_t * pty = (pty_t *)node->device;
	if (ring_buffer_unread(pty->in) > 0) {
		return 0;
	}
	return 1;
}

static int wait_pty_master(fs_node_t * node, void * process) {
	pty_t * pty = (pty_t *)node->device;
	ring_buffer_select_wait(pty->out, process);
	return 0;
}

static int wait_pty_slave(fs_node_t * node, void * process) {
	pty_t * pty = (pty_t *)node->device;
	ring_buffer_select_wait(pty->in, process);
	return 0;
}

fs_node_t * pty_master_create(pty_t * pty) {
	fs_node_t * fnode = malloc(sizeof(fs_node_t));
	memset(fnode, 0x00, sizeof(fs_node_t));

	fnode->name[0] = '\0';
	sprintf(fnode->name, "pty master");
	fnode->uid   = current_process->user;
	fnode->gid   = 0;
	fnode->mask  = 0666;
	fnode->flags = FS_PIPE;
	fnode->read  =  read_pty_master;
	fnode->write = write_pty_master;
	fnode->open  =  open_pty_master;
	fnode->close = close_pty_master;
	fnode->selectcheck = check_pty_master;
	fnode->selectwait  = wait_pty_master;
	fnode->readdir = NULL;
	fnode->finddir = NULL;
	fnode->ioctl = ioctl_pty_master;
	fnode->get_size = pty_available_output;
	fnode->ctime   = now();
	fnode->mtime   = now();
	fnode->atime   = now();

	fnode->device = pty;

	return fnode;
}

fs_node_t * pty_slave_create(pty_t * pty) {
	fs_node_t * fnode = malloc(sizeof(fs_node_t));
	memset(fnode, 0x00, sizeof(fs_node_t));

	fnode->name[0] = '\0';
	sprintf(fnode->name, "pty slave");
	fnode->uid   = current_process->user;
	fnode->gid   = 0;
	fnode->mask  = 0666;
	fnode->flags = FS_PIPE;
	fnode->read  =  read_pty_slave;
	fnode->write = write_pty_slave;
	fnode->open  =  open_pty_slave;
	fnode->close = close_pty_slave;
	fnode->selectcheck = check_pty_slave;
	fnode->selectwait  = wait_pty_slave;
	fnode->readdir = NULL;
	fnode->finddir = NULL;
	fnode->ioctl = ioctl_pty_slave;
	fnode->get_size = pty_available_input;
	fnode->ctime   = now();
	fnode->mtime   = now();
	fnode->atime   = now();

	fnode->device = pty;

	return fnode;
}

static struct dirent * readdir_pty(fs_node_t *node, uint32_t index) {
	if (index == 0) {
		struct dirent * out = malloc(sizeof(struct dirent));
		memset(out, 0x00, sizeof(struct dirent));
		out->ino = 0;
		strcpy(out->name, ".");
		return out;
	}

	if (index == 1) {
		struct dirent * out = malloc(sizeof(struct dirent));
		memset(out, 0x00, sizeof(struct dirent));
		out->ino = 0;
		strcpy(out->name, "..");
		return out;
	}

	index -= 2;

	pty_t * out_pty = NULL;
	list_t * values = hashmap_values(_pty_index);
	foreach(node, values) {
		if (index == 0) {
			out_pty = node->value;
			break;
		}
		index--;
	}
	list_free(values);

	if (out_pty) {
		struct dirent * out = malloc(sizeof(struct dirent));
		memset(out, 0x00, sizeof(struct dirent));
		out->ino = out_pty->name;
		out->name[0] = '\0';
		sprintf(out->name, "%d", out_pty->name);
		return out;
	} else {
		return NULL;
	}
}

static fs_node_t * finddir_pty(fs_node_t * node, char * name) {
	if (!name) return NULL;
	if (strlen(name) < 1) return NULL;

	int c = 0;
	for (int i = 0; name[i]; ++i) {
		if (name[i] < '0' || name[i] > '9') {
			return NULL;
		}
		c = c * 10 + name[i] - '0';
	}

	pty_t * _pty = hashmap_get(_pty_index, (void*)c);

	if (!_pty) {
		debug_print(ERROR, "Invalid PTY number: %d\n", c);
		return NULL;
	}

	return _pty->slave;
}

static fs_node_t * create_pty_dir(void) {
	fs_node_t * fnode = malloc(sizeof(fs_node_t));
	memset(fnode, 0x00, sizeof(fs_node_t));
	fnode->inode = 0;
	strcpy(fnode->name, "pty");
	fnode->mask = 0555;
	fnode->uid  = 0;
	fnode->gid  = 0;
	fnode->flags   = FS_DIRECTORY;
	fnode->read    = NULL;
	fnode->write   = NULL;
	fnode->open    = NULL;
	fnode->close   = NULL;
	fnode->readdir = readdir_pty;
	fnode->finddir = finddir_pty;
	fnode->nlink   = 1;
	fnode->ctime   = now();
	fnode->mtime   = now();
	fnode->atime   = now();
	return fnode;
}

void pty_install(void) {
	_pty_index = hashmap_create_int(10);
	_pty_dir   = create_pty_dir();

	vfs_mount("/dev/pts", _pty_dir);
}

pty_t * pty_new(struct winsize * size) {

	if (!_pty_index) {
		pty_install();
	}

	pty_t * pty = malloc(sizeof(pty_t));

	/* stdin linkage; characters from terminal → PTY slave */
	pty->in  = ring_buffer_create(TTY_BUFFER_SIZE);
	pty->out = ring_buffer_create(TTY_BUFFER_SIZE);

	pty->in->discard = 1;

	/* Master endpoint - writes go to stdin, reads come from stdout */
	pty->master = pty_master_create(pty);

	/* Slave endpoint, reads come from stdin, writes go to stdout */
	pty->slave  = pty_slave_create(pty);

	/* tty name */
	pty->name   = _pty_counter++;

	hashmap_set(_pty_index, (void*)pty->name, pty);

	if (size) {
		memcpy(&pty->size, size, sizeof(struct winsize));
	} else {
		/* Sane defaults */
		pty->size.ws_row = 25;
		pty->size.ws_col = 80;
	}

	/* Controlling and foreground processes are set to 0 by default */
	pty->ct_proc = 0;
	pty->fg_proc = 0;

	pty->tios.c_iflag = ICRNL | BRKINT;
	pty->tios.c_oflag = ONLCR | OPOST;
	pty->tios.c_lflag = ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN;
	pty->tios.c_cflag = CREAD | CS8;
	pty->tios.c_cc[VEOF]   =  4; /* ^D */
	pty->tios.c_cc[VEOL]   =  0; /* Not set */
	pty->tios.c_cc[VERASE] = '\b';
	pty->tios.c_cc[VINTR]  =  3; /* ^C */
	pty->tios.c_cc[VKILL]  = 21; /* ^U */
	pty->tios.c_cc[VMIN]   =  1;
	pty->tios.c_cc[VQUIT]  = 28; /* ^\ */
	pty->tios.c_cc[VSTART] = 17; /* ^Q */
	pty->tios.c_cc[VSTOP]  = 19; /* ^S */
	pty->tios.c_cc[VSUSP] = 26; /* ^Z */
	pty->tios.c_cc[VTIME]  =  0;

	pty->canon_buffer  = malloc(TTY_BUFFER_SIZE);
	pty->canon_bufsize = TTY_BUFFER_SIZE-2;
	pty->canon_buflen  = 0;

	return pty;
}

int pty_create(void *size, fs_node_t ** fs_master, fs_node_t ** fs_slave) {
	pty_t * pty = pty_new(size);

	*fs_master = pty->master;
	*fs_slave  = pty->slave;

	return 0;
}
