/* cheatcoin main, T13.654-T13.895 $DVS:time$ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <signal.h>
#endif
#include "system.h"
#include "address.h"
#include "block.h"
#include "crypt.h"
#include "transport.h"
#include "./dnet_main.h"
//#include "../dnet/dnet_history.h"
#include "version.h"
#include "wallet.h"
#include "init.h"
#include "common.h"
#include "commands.h"
#include "terminal.h"
#include "memory.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "json-rpc/rpc_service.h"

char *g_coinname, *g_progname;
#define coinname   g_coinname

#define ARG_EQUAL(a,b,c) strcmp(c, "") == 0 ? strcmp(a, b) == 0 : (strcmp(a, b) == 0 || strcmp(a, c) == 0)

int g_xdag_state = XDAG_STATE_INIT;
int g_xdag_testnet = 0;
int g_is_miner = 0;
time_t g_xdag_xfer_last = 0;
enum xdag_field_type g_block_header_type = XDAG_FIELD_HEAD;
struct xdag_stats g_xdag_stats;
struct xdag_ext_stats g_xdag_extstats;
int g_disable_mining = 0;

void printUsage(char* appName);

int xdag_init(int argc, char **argv, int isGui)
{
	xdag_init_path(argv[0]);

	const char *addrports[256], *bindto = 0, *pool_arg = 0, *miner_address = 0;
	int deamon_flags = 0, n_addrports = 0, is_miner = 1, level, is_rpc = 0, rpc_port = 0;
	
	memset(addrports, 0, 256);
	
#if !defined(_WIN32) && !defined(_WIN64)
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGWINCH, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
#endif

	char *filename = xdag_filename(argv[0]);

	g_progname = strdup(filename);
	g_coinname = strdup(filename);
	free(filename);

	xdag_str_toupper(g_coinname);
	xdag_str_tolower(g_progname);

	if (!isGui) {
		printf("%s client/server, version %s.\n", g_progname, XDAG_VERSION);
	}

	if (argc <= 1) {
		printUsage(argv[0]);
		return 0;
	}

	for (int i = 1; i < argc; ++i) {
		if (argv[i][0] != '-') {
			if ((!argv[i][1] || argv[i][2]) && strchr(argv[i], ':')) {
				is_miner = 1;
				pool_arg = argv[i];
			} else {
				printUsage(argv[0]);
				return 0;
			}
			continue;
		}
		
		if (ARG_EQUAL(argv[i], "-a", "")) { /* miner address */
			if (++i < argc) miner_address = argv[i];
		} else if(ARG_EQUAL(argv[i], "-d", "")) { /* daemon mode */
#if !defined(_WIN32) && !defined(_WIN64)
			deamon_flags |= 0x1;
#endif
		} else if(ARG_EQUAL(argv[i], "-h", "")) { /* help */
			printUsage(argv[0]);
			return 0;
		} else if(ARG_EQUAL(argv[i], "-i", "")) { /* interactive mode */
			return terminal();
		} else if(ARG_EQUAL(argv[i], "-l", "")) { /* list balance */
			return out_balances();
		} else if(ARG_EQUAL(argv[i], "-t", "")) { /* connect test net */
			g_xdag_testnet = 1;
		} else if(ARG_EQUAL(argv[i], "-v", "")) { /* log level */
			if (++i < argc && sscanf(argv[i], "%d", &level) == 1) {
				xdag_set_log_level(level);
			} else {
				printf("Illevel use of option -v\n");
				return -1;
			}
		} else if(ARG_EQUAL(argv[i], "", "-rpc-enable")) { /* enable JSON-RPC service */
			is_rpc = 1;
		} else if(ARG_EQUAL(argv[i], "", "-rpc-port")) { /* set JSON-RPC service port */
			if(++i < argc && sscanf(argv[i], "%d", &rpc_port) == 1) {
				if(rpc_port < 0 || rpc_port > 65535) {
					printf("RPC port is invalid, set to default.\n");
					rpc_port = 0;
				}
			}
		} else {
			printUsage(argv[0]);
			return 0;
		}
	}

	// use RAM as default config for miner
	if(is_miner) {
		xdag_mem_tempfile_path("RAM");
	}

	g_xdag_pool = !is_miner; // move to here to avoid Data Race
	g_is_miner = is_miner;

	if(g_xdag_testnet) {
		g_block_header_type = XDAG_FIELD_HEAD_TEST; //block header has the different type in the test network
	}

	memset(&g_xdag_stats, 0, sizeof(g_xdag_stats));
	memset(&g_xdag_extstats, 0, sizeof(g_xdag_extstats));

	xdag_mess("Starting %s, version %s", g_progname, XDAG_VERSION);
	xdag_mess("Starting dnet transport...");
	printf("Transport module: ");
	if (xdag_transport_start(deamon_flags, bindto, n_addrports, addrports)) return -1;
//	if(dnet_crypt_init(DNET_VERSION)) {
//		xdag_err("dnet crypt init failed...");
//		return -1;
//	}

	if (xdag_log_init()) return -1;

	xdag_mess("Initializing cryptography...");
	if (xdag_crypt_init(1)) return -1;
	xdag_mess("Reading wallet...");
	if (xdag_wallet_init()) return -1;
	xdag_mess("Initializing addresses...");
	if (xdag_address_init()) return -1;
	if(is_rpc) {
		xdag_mess("Initializing RPC service...");
		if(!!xdag_rpc_service_init(rpc_port)) return -1;
	}
	xdag_mess("Starting blocks engine...");
	if (xdag_blocks_start(!g_is_miner, 0, !!miner_address)) return -1;

	xdag_mess("Starting engine...");
	if(xdag_initialize_mining(pool_arg, miner_address)) return -1;

	if (!isGui) {
		if (!is_miner || (deamon_flags & 0x1) > 0) {
			xdag_mess("Starting terminal server...");
			pthread_t th;
			const int err = pthread_create(&th, 0, &terminal_thread, 0);
			if(err != 0) {
				printf("create terminal_thread failed, error : %s\n", strerror(err));
				return -1;
			}
		}

		startCommandProcessing(deamon_flags);
	}

	return 0;
}

int xdag_set_password_callback(int(*callback)(const char *prompt, char *buf, unsigned size))
{
    return xdag_user_crypt_action((uint32_t *)(void *)callback, 0, 0, 6);
}

void printUsage(char* appName)
{
	printf("Usage: %s flags [pool_ip:port]\n"
		"If pool_ip:port argument is given, then the node operates as a miner.\n"
		"Flags:\n"
		"  -a address     - specify your address to use in the miner\n"
		"  -d             - run as daemon (default is interactive mode)\n"
		"  -h             - print this help\n"
		"  -i             - run as interactive terminal for daemon running in this folder\n"
		"  -l             - output non zero balances of all accounts\n"
		"  -t             - connect to test net (default is main net)\n"
		"  -v N           - set loglevel to N\n"
		"  -rpc-enable    - enable JSON-RPC service\n"
		"  -rpc-port      - set HTTP JSON-RPC port (default is 7677)\n"
		, appName);
}
