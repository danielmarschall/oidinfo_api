// **************************************************
// ** OID CSV Lookup Server 2.0.1                  **
// ** (c) 2016-2026 ViaThinkSoft, Daniel Marschall **
// **************************************************

// You can locally test it using:
// telnet 127.0.0.1 49500

// TODO: log verbosity + datetime
// TODO: 2019-02-24 service was booted together with the system, and i got "0 OIDs loaded". why???
// TODO: create vnag monitor that checks if this service is OK

#include <fstream>
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <netdb.h>
#include <unistd.h>
#include <sstream>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <cstring>
#include <unordered_set>
#include <atomic>
#include <csignal>

#define PORT    49500
#define MAXMSG  512

#define CONNECTION_TIMEOUT 60
#define RATE_LIMIT_QUERIES 1000000

// In seconds. 21600 = 6 hours
#define CSVRELOADINTERVAL 21600

using namespace std;

int sock;
time_t csvLoad = time(NULL);

// --- Code for hot-swapping data structures

unordered_set<string> linesA;
unordered_set<string> linesB;

atomic<unordered_set<string>*> activeLines(&linesA);

int loadfile(const string &filename, unordered_set<string> &target) {
	int cnt = 0;

	target.clear();

	ifstream infile(filename);
	string line;

	while (getline(infile, line)) {
		target.insert(line);
		++cnt;
	}

	infile.close();

	printf("Loaded %d OIDs from %s\n", cnt, filename.c_str());
	return cnt;
}

int loadCSVs() {
	unordered_set<string>* current = activeLines.load();

	unordered_set<string>* buildTarget = (current == &linesA) ? &linesB : &linesA;

	int count = loadfile("oid_table.csv", *buildTarget);

	// Swap active set
	activeLines.store(buildTarget);

	return count;
}

// --- Data handling logic

bool stringAvailable(const string &str) {
	unordered_set<string>* current = activeLines.load();
	return current->find(str) != current->end();
}

// --- Connection handling logic

struct con_descriptor {
	time_t              last_activity;
	sockaddr_storage    clientname;
	int                 queries;
	time_t              connect_ts;
	char                inbuf[MAXMSG];
	int                 inbuf_len;
};

con_descriptor cons[FD_SETSIZE];

const char* addr_to_string(const sockaddr *sa, char *buf, size_t len, uint16_t *port) {
	if (sa->sa_family == AF_INET) {
		const sockaddr_in *v4 = (const sockaddr_in*)sa;
		if (!inet_ntop(AF_INET, &v4->sin_addr, buf, len))
			snprintf(buf, len, "?");
		if (port) *port = ntohs(v4->sin_port);
	} else if (sa->sa_family == AF_INET6) {
		const sockaddr_in6 *v6 = (const sockaddr_in6*)sa;
		if (!inet_ntop(AF_INET6, &v6->sin6_addr, buf, len))
			snprintf(buf, len, "?");
		if (port) *port = ntohs(v6->sin6_port);
	} else {
		snprintf(buf, len, "?");
		if (port) *port = 0;
	}

	return buf;
}

int read_from_client(int filedes) {
	size_t bufsize = sizeof(cons[filedes].inbuf);
	size_t len = cons[filedes].inbuf_len;

	/* prüfen ob noch Platz für mindestens 1 Byte + '\0' ist */
	if (len >= bufsize - 1)
		return -1;

	size_t max_read = bufsize - 1 - len;

	ssize_t nbytes = read(
		filedes,
		cons[filedes].inbuf + len,
		max_read
	);

	if (nbytes <= 0)
		return -2;

	cons[filedes].inbuf_len = len + nbytes;
	cons[filedes].inbuf[cons[filedes].inbuf_len] = '\0';

	// Only work on it once \n is there
	char *nl = strchr(cons[filedes].inbuf, '\n');
	if (!nl) return 0; // not complete yet. wait.
	*nl = '\0';

	// Strip trailing \r (Windows clients)
	if (nl > cons[filedes].inbuf && *(nl-1) == '\r') *(nl-1) = '\0';

	char ip[INET6_ADDRSTRLEN];
	uint16_t port;
	addr_to_string((sockaddr*)&cons[filedes].clientname, ip, sizeof(ip), &port);

	if (strcmp(cons[filedes].inbuf, "bye") == 0) {
		fprintf(stdout, "%s:%d[%d] Client said good bye.\n", ip, port, filedes);
		return -3;
	} else if (strcmp(cons[filedes].inbuf, "reload") == 0) {
		sockaddr *sa = (sockaddr*)&cons[filedes].clientname;

		bool is_local = false;
		if (sa->sa_family == AF_INET) {
			sockaddr_in *v4 = (sockaddr_in*)sa;
			uint32_t addr = ntohl(v4->sin_addr.s_addr);
			is_local = ((addr & 0xFF000000) == 0x7F000000); // 127.0.0.0/8
		} else if (sa->sa_family == AF_INET6) {
			sockaddr_in6 *v6 = (sockaddr_in6*)sa;
			if (IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr)) {
				is_local = true; // ::1
			} else if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
				uint32_t ipv4;
				memcpy(&ipv4, &v6->sin6_addr.s6_addr[12], 4);
				ipv4 = ntohl(ipv4);
				if ((ipv4 & 0xFF000000) == 0x7F000000)
					is_local = true;
			}
		}

		if (!is_local) {
			fprintf(stdout, "%s:%d[%d] Reload rejected (not localhost)\n", ip, port, filedes);
			write(filedes, "FORBIDDEN\n", strlen("FORBIDDEN\n"));
			return -4;
		} else {
			fprintf(stdout, "%s:%d[%d] Client requested a reload.\n", ip, port, filedes);
			loadCSVs();
			csvLoad = time(NULL);
			if (write(filedes, "OK\n", 3) < 0) return -5;

			// Nach Verarbeitung: Rest nach vorne schieben
			int processed = (nl - cons[filedes].inbuf) + 1; // +1 für das \n
			int remaining = cons[filedes].inbuf_len - processed;
			if (remaining > 0)
			memmove(cons[filedes].inbuf, nl + 1, remaining);
			cons[filedes].inbuf_len = remaining;

			return 0;
		}
	} else if (strcmp(cons[filedes].inbuf, "terminate") == 0) {
		sockaddr *sa = (sockaddr*)&cons[filedes].clientname;

		bool is_local = false;
		if (sa->sa_family == AF_INET) {
			sockaddr_in *v4 = (sockaddr_in*)sa;
			uint32_t addr = ntohl(v4->sin_addr.s_addr);
			is_local = ((addr & 0xFF000000) == 0x7F000000); // 127.0.0.0/8
		} else if (sa->sa_family == AF_INET6) {
			sockaddr_in6 *v6 = (sockaddr_in6*)sa;
			if (IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr)) {
				is_local = true; // ::1
			} else if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
				uint32_t ipv4;
				memcpy(&ipv4, &v6->sin6_addr.s6_addr[12], 4);
				ipv4 = ntohl(ipv4);
				if ((ipv4 & 0xFF000000) == 0x7F000000)
					is_local = true;
			}
		}

		if (!is_local) {
			fprintf(stdout, "%s:%d[%d] Termination rejected (not localhost)\n", ip, port, filedes);
			write(filedes, "FORBIDDEN\n", strlen("FORBIDDEN\n"));
			return -6;
		} else {
			fprintf(stdout, "%s:%d[%d] Client requested termination of service.\n", ip, port, filedes);
			loadCSVs();
			csvLoad = time(NULL);
			if (write(filedes, "OK\n", 3) < 0) return -7;

			// Nach Verarbeitung: Rest nach vorne schieben
			int processed = (nl - cons[filedes].inbuf) + 1; // +1 für das \n
			int remaining = cons[filedes].inbuf_len - processed;
			if (remaining > 0)
			memmove(cons[filedes].inbuf, nl + 1, remaining);
			cons[filedes].inbuf_len = remaining;

			return -999; // exits program
		}
	} else {
		cons[filedes].queries++;

		if (cons[filedes].queries > RATE_LIMIT_QUERIES) {
			fprintf(stdout, "%s:%d[%d] Client reached rate limit (%d).\n", ip, port, filedes, RATE_LIMIT_QUERIES);
			if (write(filedes, "RATE LIMIT REACHED\n", strlen("RATE LIMIT REACHED\n")) < 0) return -8;
			return -9;
		}

		for (uint i=0; i<sizeof(cons[filedes].inbuf); ++i) {
			if (cons[filedes].inbuf[i] == 0) break;
			if (!((cons[filedes].inbuf[i] >= '0') && (cons[filedes].inbuf[i] <= '9')) && !(cons[filedes].inbuf[i] == '.')) {
				fprintf(stdout, "%s:%d[%d] Client sent an invalid request.\n", ip, port, filedes);
				return -10;
			}
		}

		fprintf(stdout, "%s:%d[%d] Query #%d: %s\n", ip, port, filedes, cons[filedes].queries, cons[filedes].inbuf);

		if (stringAvailable(cons[filedes].inbuf)) {
			if (write(filedes, "1\n", 2) < 0) return -11;
		} else {
			if (write(filedes, "0\n", 2) < 0) return -12;
		}

		// Nach Verarbeitung: Rest nach vorne schieben
		int processed = (nl - cons[filedes].inbuf) + 1; // +1 für das \n
		int remaining = cons[filedes].inbuf_len - processed;
		if (remaining > 0)
		memmove(cons[filedes].inbuf, nl + 1, remaining);
		cons[filedes].inbuf_len = remaining;

		return 0;
	}
}

int fd_set_isset_count(const fd_set &my_fd_set) {
	int cnt = 0;
	for (int fd = 0; fd < FD_SETSIZE; ++fd) {
		if (FD_ISSET(fd, &my_fd_set)) {
			++cnt;
		}
	}
	return cnt;
}

void initConsArray() {
	for (int i=0; i<FD_SETSIZE; ++i) {
		cons[i].last_activity = 0;
		memset(&cons[i].clientname, 0, sizeof(sockaddr_storage));
		cons[i].queries = 0;
		cons[i].inbuf_len = 0;
		cons[i].inbuf[0] = '\0';
	}
}

int make_socket(uint16_t port) {
	/* Create the socket. */
	int sock = socket(AF_INET6, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	/* Apply settings */
	int opt = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		fprintf(stderr, "ERROR: setsockopt(SO_REUSEADDR) failed");
		exit(EXIT_FAILURE);
	}

	//if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
	//	fprintf(stderr, "ERROR: setsockopt(SO_REUSEPORT) failed");
	//	exit(EXIT_FAILURE);
	//}

	int off = 0;
	if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
		fprintf(stderr, "ERROR: setsockopt(IPV6_V6ONLY) failed");
		exit(EXIT_FAILURE);
	}

	/* Bind to IPv4 any */
	/*
	name.sin_family = AF_INET;
	name.sin_port = htons (port);
	name.sin_addr.s_addr = htonl (INADDR_ANY);
	if (bind (sock, (struct sockaddr *) &name, sizeof(name)) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}
	*/

	/* Bind to IPv6 any */
	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_any;
	if (bind (sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	return sock;
}

void handle_sigint(int sig) {
	printf("Shutting down...\n");
	close(sock);
	exit(0);
}

// --- Main method

int main(void) {
//	extern int make_socket(uint16_t port);
	fd_set active_fd_set, read_fd_set;

	fprintf(stdout, "OID CSV Lookup Server 2.0.1 (c)2016-2026 ViaThinkSoft\n");
	fprintf(stdout, "Listening at port: %d\n", PORT);
	fprintf(stdout, "Max connections: %d\n", FD_SETSIZE);

	// write() auf eine geschlossene Connection schickt SIGPIPE -> Prozess stirbt.
	signal(SIGPIPE, SIG_IGN);

	// allow ctrl+c
	signal(SIGINT, handle_sigint);

	initConsArray();

	int loadedOIDs = loadCSVs();

	/* Create the socket and set it up to accept connections. */
	sock = make_socket(PORT);
	if (listen(sock, SOMAXCONN) < 0) {
		perror("listen");
		exit(EXIT_FAILURE);
	}

	/* Initialize the set of active sockets. */
	FD_ZERO(&active_fd_set);
	FD_SET(sock, &active_fd_set);

	while (1) {
		/* Block until input arrives on one or more active sockets. */
		read_fd_set = active_fd_set;

		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;  // Not init'ing this can cause strange errors

		int retval = select(FD_SETSIZE, &read_fd_set, NULL, NULL, &tv);

		if (retval < 0) {
			perror("select");
			exit(EXIT_FAILURE);
		} else if (retval == 0) {
			// fprintf(stdout, "Nothing received\n");
		} else {
			/* Service all the sockets with input pending. */
			for (int i=0; i < FD_SETSIZE; ++i) {
				if (FD_ISSET (i, &read_fd_set)) {
					if (i == sock) {
						/* Connection request on original socket. */
						int new_fd;
						/*struct sockaddr_in*/sockaddr_storage clientname;
						socklen_t size = sizeof(clientname);
						new_fd = accept(sock, (struct sockaddr *) &clientname, &size);
						char ip[INET6_ADDRSTRLEN];
						uint16_t port;
						addr_to_string((sockaddr*)&clientname, ip, sizeof(ip), &port);
						if (new_fd < 0) {
							perror("accept");
							exit(EXIT_FAILURE);
						}
						if (new_fd >= FD_SETSIZE) {
							fprintf(stderr, "%s:%d[%d] new_fd reached cons[FD_SETSIZE] limit\n", ip, port, new_fd);
							close(new_fd);
							continue;
						}
						FD_SET(new_fd, &active_fd_set);

						cons[new_fd].clientname = clientname;
						cons[new_fd].connect_ts = time(NULL);
						cons[new_fd].inbuf_len = 0;
						cons[new_fd].inbuf[0] = '\0';

						if (loadedOIDs == 0) {
							loadedOIDs = loadCSVs();
							if (loadedOIDs == 0) {
								fprintf(stderr, "%s:%d[%d] Service temporarily unavailable (OID list empty)\n", ip, port, new_fd);
								close(new_fd);
								FD_CLR(new_fd, &active_fd_set);
								continue;
							}
						}

						if (fd_set_isset_count(active_fd_set)-1 > FD_SETSIZE) { // -1 is because we need to exclude the listening socket (i=sock) which is not a connected client
							fprintf(stderr, "%s:%d[%d] Rejected because too many connections are open\n", ip, port, new_fd);
							close(new_fd);
							FD_CLR(new_fd, &active_fd_set);
							continue;
						} else {
							fprintf(stdout, "%s:%d[%d] Connected\n", ip, port, new_fd);
						}

						cons[new_fd].last_activity = time(NULL);
						cons[new_fd].queries = 0;
					} else {
						/* Data arriving on an already-connected socket. */
						cons[i].last_activity = time(NULL);
						int ret = read_from_client(i);
						if (ret == -999) {
							break;
						} else if (ret < 0) {
							char ip[INET6_ADDRSTRLEN];
							uint16_t port;
							addr_to_string((sockaddr*)&cons[i].clientname, ip, sizeof(ip), &port);

							fprintf(stdout, "%s:%d[%d] Connection closed after %d queries in %lu seconds.\n", ip, port, i, cons[i].queries, time(NULL)-cons[i].connect_ts);
							close(i);
							FD_CLR(i, &active_fd_set);
							continue;
						}
					}
				}
			}
		}

		/* Check if we need to reload the CSV */
		if (time(NULL)-csvLoad >= CSVRELOADINTERVAL) {
			loadCSVs();
			csvLoad = time(NULL);
		}

		/* Check if we can close connections due to timeout */
		for (int i=0; i < FD_SETSIZE; ++i) {
			if (FD_ISSET(i, &active_fd_set)) {
				if (i == sock) continue;
				if (time(NULL)-cons[i].last_activity >= CONNECTION_TIMEOUT) {
					char ip[INET6_ADDRSTRLEN];
					uint16_t port;
					addr_to_string((sockaddr*)&cons[i].clientname, ip, sizeof(ip), &port);

					fprintf(stdout, "%s:%d[%d] Connection closed after %d queries in %lu seconds due to timeout.\n", ip, port, i, cons[i].queries, time(NULL)-cons[i].connect_ts);
					close(i);
					FD_CLR(i, &active_fd_set);
				}
			}
		}
	}
}
