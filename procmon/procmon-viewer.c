#include "procmon-viewer.h"

unsigned int procmon_nodes = 0;
unsigned long int total_nodes = 0;
syscall_intercept_info_node *head, *curr, *tail;

int main(int argc, char **argv){
	/*Random vars*/
	int ch;
	FILE *file;

	/*Event loop related vars*/
	int sock_fd, stdin_fd, efd;
	struct epoll_event 
	event = { 
		.events = 0 
	}, 
	events[2] = { 
		{ .events = 0 },
		{ .events = 0 }
	};

	/*NetLink related vars*/
	struct iovec iov;
	struct msghdr msg;
	struct nlmsghdr *nlh = NULL;

	while((ch = getopt(argc, argv, "clusevp:")) != -1){
		switch(ch){
			#ifndef __NO_KMOD__

			int ret;

			case 'c':
				ret = check(PROCMON_MODULE_NAME);
				if(ret == 0){
					printf("Procmon kernel module is not loaded.\n");
				}else if(ret == 1){
					printf("Procmon kernel module is loaded.\n");
				}
				return 0;

			case 'l':
				if(load(PROCMON_MODULE_PATH) == 0){
					printf("Procmon kernel module successfully loaded.\n");
				}
				return 0;

			case 'u':
				if(unload(PROCMON_MODULE_NAME) == 0){
					printf("Procmon kernel module successfully unloaded.\n");
				}
				return 0;

			case 's':
				if(start() == 0){
					printf("Procmon kernel module successfully started.\n");
				}
				return 0;

			case 'e':
				if(stop() == 0){
					printf("Procmon kernel module successfully stopped.\n");
				}
				return 0;

			#endif

			case 'v':
				printf("Procmon %g\n", PROCMON_VERSION);
				break;

			case '?':
				printf(
					"Possible options are:\n\t"
						#ifndef __NO_KMOD__
						"'c' - Check if procmon kernel module is loaded.\n\t"
						"'l' - Load procmon kernel module.\n\t"
						"'u' - Unload procmon kernel module.\n\t"
						"'s' - Start procmon kernel module hijack.\n\t"
						"'e' - End procmon kernel module hijack.\n\t"
						#endif
						"'v' - Show procmon version.\n"
				);
				return 1;

			default:
				break;
		}
	}

	/*Initialize NetLink msg headers and get socket file descriptor*/
	sock_fd = net_init(&nlh, &msg, &iov);
	if(sock_fd == -1){
		printf("Error starting NetLink.\n");
		return -1;
	}
	//fcntl(sock_fd, F_SETFL, fcntl(sock_fd, F_GETFL, 0) | O_NONBLOCK); //Do we need this?

	/*Get STDIN file descriptor*/
	stdin_fd = fcntl(STDIN_FILENO,  F_DUPFD, 0);

	/*Set our client PID in Procmon*/
	file = fopen("/proc/sys/procmon/client_pid", "w");
	if(file){
		fprintf(file, "%d", getpid());
		fclose(file);
	}

	/*Make SEGFAULTs play nice with NCURSES*/
	signal(SIGSEGV, do_segfault);

	/*Create event loop*/
	efd = epoll_create1(0);
	if(efd == -1){
		printf("Error creating epoll\n");
		return -1;
	}

	event.data.fd = sock_fd;
	event.events = EPOLLIN;
	if(epoll_ctl(efd, EPOLL_CTL_ADD, sock_fd, &event) == -1){
		printf("Error adding socket fd to epoll\n");
		return -1;
	}

	event.data.fd = stdin_fd;
	event.events = EPOLLIN;
	if(epoll_ctl(efd, EPOLL_CTL_ADD, stdin_fd, &event) == -1){
		printf("Error adding stdin fd to epoll\n");
		return -1;
	}

	/*Keep track on the data we have*/
	head = new(sizeof(syscall_intercept_info_node));
	if(!head){
		printf("Error allocating memory for data list\n");
		return -1;
	}
	head->prev = head->next = NULL;
	head->i = NULL;
	tail = curr = head;

	/*Init ncurses window*/
	init_ncurses();

	/*React on resize*/
	signal(SIGWINCH, do_resize);

	/*Get current positiona and size*/
	calc_w_size_pos();
	refresh();
	create_win_data_data_box();

	/*Start even loop*/
	while(1){
		int n, i, quit = 0;
		n = epoll_wait(efd, events, MAXEVENTS, -1);
		for(i = 0; i < n; i++){
			if(events[i].events & EPOLLIN){
				if(events[i].data.fd == sock_fd){
					/*Read from NetLink and add data*/
					add_data( read_from_socket(sock_fd, nlh, msg) );

					//Don't draw if there's nothing new to draw
					if(curr == tail){
						draw_data(curr);
					}
				}else if(events[i].data.fd == stdin_fd){
					if(read_from_kb() < 0){
						quit = 1;
						break;
					}
				}
			}else if(events[i].events & (EPOLLHUP | EPOLLERR)){
				continue;
			}
		}

		if(quit){
			break;
		}
	}

	close(sock_fd);

	/*Free all the memory we allocated for the data*/
	syscall_intercept_info_node *in = head, *tmp;
	while(!in->next){
		free_data(in->i);

		tmp = in;
		in = in->next;

		del(tmp);
	}

	/*Exit*/
	endwin();

	file = fopen("/proc/sys/procmon/sent_msgs", "rw");
	if(file){
		fscanf(file, "%u", &procmon_nodes);
		fprintf(file, "%u", 0); //Restart counter
		fclose(file);
	}
	printf("Received messages: %lu (Should be +/-%u)\n", total_nodes, procmon_nodes);

	return 0;
}

void do_segfault(){
	endwin();
	abort();
}

void add_data(syscall_info *i){
	syscall_intercept_info_node *in, *tmp;

	if(!i){
		return;
	}

	if(head->i == NULL){
		in = head;
		in->i = i;
	}else{

		if(total_nodes >= MEM_LIMIT){
			tmp = head;
			head = head->next;
			free_data(tmp->i);
			del(tmp);
			total_nodes--;
		}

		in = calloc(sizeof(syscall_intercept_info_node), 1);
		in->prev = tail;
		in->i = i;
		tail->next = in;
		tail = in;
		tail->next = NULL;
	}

	total_nodes++;

	//Auto-scroll on next draw!
	if(curr == tail->prev){
		curr = tail;
	}
}

void free_data(syscall_info *i){
	del(i->pname);
	del(i->operation);
	del(i->path);
	del(i->result);
	del(i->details);
	del(i);
}