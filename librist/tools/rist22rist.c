/* librist. Copyright © 2020 SipRadius LLC. All right reserved.
 * Author: Gijs Peskens
 * Author: Sergio Ammirata, Ph.D. <sergio@ammirata.net>
 * Modified: CariTech Solutions - Added multi-peer, NPD, SSRC passthrough
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* rist22rist - Enhanced RIST gateway with multi-peer bonding, NPD, and SSRC passthrough */

#include <librist/librist.h>
#include "librist/version.h"
#include "risturlhelp.h"
#include "config.h"
#if HAVE_SRP_SUPPORT
#include "librist/librist_srp.h"
#include "srp_shared.h"
#endif
#if HAVE_PROMETHEUS_SUPPORT
#include "prometheus-exporter.h"
#endif
#include "vcs_version.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "getopt-shim.h"
#include <assert.h>
#include <signal.h>
#ifdef __unix
#include <unistd.h>
#endif
#include "oob_shared.h"

#define RIST22RIST_VERSION "1.0"
#define MAX_INPUT_URLS 16
#define MAX_OUTPUT_URLS 16

struct rist_sender_args {
	char* cname;
	char* shared_secret;
	char* output_urls[MAX_OUTPUT_URLS];
	int output_url_count;
	uint16_t dst_port;
	enum rist_log_level loglevel;
	int encryption_type;
	uint32_t flow_id;
	int statsinterval;
	int npd_enabled;
	int ssrc_passthrough;
};

struct rist_cb_arg {
	uint16_t src_port;
	uint16_t dst_port;
	struct rist_ctx *sender_ctx;
	struct rist_sender_args *client_args;
	uint32_t current_flow_id;
};

static int keep_running = 1;
static struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;

#if HAVE_PROMETHEUS_SUPPORT
struct rist_prometheus_stats *prom_stats_ctx = NULL;
bool prometheus_httpd = false;
bool enable_prometheus = false;
char *prometheus_tags = NULL;
uint16_t prometheus_port = 9100;
char *prometheus_ip = NULL;
#endif

static struct option long_options[] = {
{ "inurl",             required_argument, NULL, 'i' },
{ "outurl",            required_argument, NULL, 'o' },
{ "secret",            required_argument, NULL, 's' },
{ "encryption-type",   required_argument, NULL, 'e' },
{ "cname",             required_argument, NULL, 'N' },
{ "statsinterval",     required_argument, NULL, 'S' },
{ "verbose-level",     required_argument, NULL, 'v' },
{ "remote-logging",    required_argument, NULL, 'r' },
{ "profile",           required_argument, NULL, 'p' },
{ "npd",               no_argument,       NULL, 'n' },
{ "passthrough-ssrc",  no_argument,       NULL, 'P' },
#if HAVE_SRP_SUPPORT
{ "srpfile",           required_argument, NULL, 'F' },
#endif
#if HAVE_PROMETHEUS_SUPPORT
{ "enable-metrics",    no_argument,       NULL, 'M' },
{ "metrics-tags",      required_argument, NULL, 1 },
#if HAVE_LIBMICROHTTPD
{ "metrics-http",      no_argument,       (int*)&prometheus_httpd, true },
{ "metrics-port",      required_argument, NULL, 2 },
{ "metrics-ip",        required_argument, NULL, 3 },
#endif
#endif
{ "help",              no_argument,       NULL, 'h' },
{ 0, 0, 0, 0 },
};

const char help_str[] = "Usage: %s [OPTIONS] \nWhere OPTIONS are:\n"
"       -i | --inurl ADDRESS:PORT              * | Input URL (can specify multiple for bonding)         |\n"
"       -o | --outurl ADDRESS:PORT             * | Output URL (can specify multiple for redundancy)     |\n"
"       -s | --secret PWD                        | Pre-shared encryption secret                         |\n"
"       -e | --encryption-type TYPE              | Encryption type (0 = none, 1 = AES-128, 2 = AES-256) |\n"
"       -S | --statsinterval value (ms)          | Interval at which stats get printed, 0 to disable    |\n"
"       -N | --cname identifier                  | Manually configured identifier                       |\n"
"       -v | --verbose-level value               | To disable logging: -1, log levels match syslog      |\n"
"       -r | --remote-logging IP:PORT            | Send logs and stats to this IP:PORT using udp        |\n"
"       -p | --profile number                    | Rist receive profile (0 = simple, 1 = main, 2 = adv) |\n"
"       -n | --npd                               | Enable Null Packet Deletion on output                |\n"
"       -P | --passthrough-ssrc                  | Pass through source SSRC (for multi-server bonding)  |\n"
#if HAVE_SRP_SUPPORT
"       -F | --srpfile filepath                  | SRP authentication file (use ristsrppasswd to create)|\n"
#endif
#if HAVE_PROMETHEUS_SUPPORT
"       -M | --enable-metrics                    | Enable OpenMetrics/Prometheus compatible metrics     |\n"
"          | --metrics-tags                      | Additional tags to add to the metrics                |\n"
#if HAVE_LIBMICROHTTPD
"          | --metrics-http                      | Start HTTP Server to expose metrics                  |\n"
"          | --metrics-port                      | Port for metrics HTTP server (default: 9100)         |\n"
"          | --metrics-ip                        | IP for metrics HTTP server (default: 0.0.0.0)        |\n"
#endif
#endif
"       -h | --help                              | Show this help                                       |\n"
"       -u | --help-url                          | Show all the possible url options                    |\n"
"\n"
"   * == mandatory value (at least one required)\n"
"\n"
"Multi-peer Examples:\n"
"  Input bonding (hitless failover from source):\n"
"    %s -i 'rist://@:5000?cname=circuitA' -i 'rist://@:5001?cname=circuitB' -o 'rist://dest:6000'\n"
"\n"
"  Output redundancy (send to multiple servers):\n"
"    %s -i 'rist://@:5000' -o 'rist://server1:6000' -o 'rist://server2:6001'\n"
"\n"
"  Full gateway with NPD and SSRC passthrough:\n"
"    %s -i 'rist://@:5000' -i 'rist://@:5001' -o 'rist://s1:6000' -o 'rist://s2:6001' -n -P\n"
"\n"
"Default values: %s \n"
"       --profile 0               \\\n"
"       --statsinterval 1000      \\\n"
"       --verbose-level 6         \n";

#if HAVE_SRP_SUPPORT
	char *srpfile = NULL;
#endif

static void usage(char *cmd)
{
	rist_log(&logging_settings, RIST_LOG_INFO, "%s\n%s version %s libRIST library: %s API version: %s\n",
		cmd, help_str, RIST22RIST_VERSION, librist_version(), librist_api_version());
	exit(1);
}

static int cb_auth_connect(void *arg, const char* connecting_ip, uint16_t connecting_port, const char* local_ip, uint16_t local_port, struct rist_peer *peer)
{
	struct rist_ctx *receiver_ctx = (struct rist_ctx *)arg;
	uint16_t buffer[250];
	char message[200];
	int message_len = snprintf(message, 200, "auth,%s:%d,%s:%d", connecting_ip, connecting_port, local_ip, local_port);
	int ret = oob_build_api_payload(buffer, (char *)connecting_ip, (char *)local_ip, message, message_len);
	rist_log(&logging_settings, RIST_LOG_INFO,"Peer has been authenticated, sending oob/api message: %s\n", message);
	struct rist_oob_block oob_block;
	oob_block.peer = peer;
	oob_block.payload = buffer;
	oob_block.payload_len = ret;
	rist_oob_write(receiver_ctx, &oob_block);
	return 0;
}

static int cb_auth_disconnect(void *arg, struct rist_peer *peer)
{
	(void)peer;
	struct rist_ctx *ctx = (struct rist_ctx *)arg;
	(void)ctx;
	return 0;
}

static int cb_recv_oob(void *arg, const struct rist_oob_block *oob_block)
{
	struct rist_ctx *ctx = (struct rist_ctx *)arg;
	(void)ctx;
	int message_len = 0;
	char *message = oob_process_api_message((int)oob_block->payload_len, (char *)oob_block->payload, &message_len);
	if (message) {
		rist_log(&logging_settings, RIST_LOG_INFO,"Out-of-band api data received: %.*s\n", message_len, message);
	}
	return 0;
}

static int cb_stats(void *arg, const struct rist_stats *stats_container) {
#if HAVE_PROMETHEUS_SUPPORT
	if (prom_stats_ctx != NULL)
		rist_prometheus_parse_stats(prom_stats_ctx, stats_container, (uintptr_t)arg);
#else
	(void)arg;
#endif
	rist_log(&logging_settings, RIST_LOG_INFO, "%s\n", stats_container->stats_json);
	rist_stats_free(stats_container);
	return 0;
}

static struct rist_ctx* setup_rist_sender(struct rist_sender_args *setup) {
	struct rist_ctx *ctx;

	rist_log(&logging_settings, RIST_LOG_INFO, "Setting up sender with %d output peer(s)\n", setup->output_url_count);

	if (rist_sender_create(&ctx, RIST_PROFILE_MAIN, setup->flow_id, &logging_settings) != 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not create rist sender context\n");
		exit(1);
	}

	int rist = rist_auth_handler_set(ctx, cb_auth_connect, cb_auth_disconnect, ctx);
	if (rist < 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not initialize rist auth handler\n");
		exit(1);
	}

	if (rist_oob_callback_set(ctx, cb_recv_oob, ctx) == -1) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not add enable out-of-band data\n");
		exit(1);
	}

	if (setup->statsinterval) {
		rist_stats_callback_set(ctx, setup->statsinterval, cb_stats, NULL);
	}

	// Enable NPD if requested
	if (setup->npd_enabled) {
		rist_sender_npd_enable(ctx);
		rist_log(&logging_settings, RIST_LOG_INFO, "Null Packet Deletion (NPD) enabled on output\n");
	}

	// Add all output peers
	int keysize = setup->encryption_type * 128;

	for (int i = 0; i < setup->output_url_count; i++) {
		struct rist_peer_config app_peer_config = {
			.version = RIST_PEER_CONFIG_VERSION,
			.virt_dst_port = setup->dst_port,
			.recovery_mode = RIST_DEFAULT_RECOVERY_MODE,
			.recovery_maxbitrate = RIST_DEFAULT_RECOVERY_MAXBITRATE,
			.recovery_maxbitrate_return = RIST_DEFAULT_RECOVERY_MAXBITRATE_RETURN,
			.recovery_length_min = RIST_DEFAULT_RECOVERY_LENGTH_MIN,
			.recovery_length_max = RIST_DEFAULT_RECOVERY_LENGTH_MAX,
			.recovery_reorder_buffer = RIST_DEFAULT_RECOVERY_REORDER_BUFFER,
			.recovery_rtt_min = RIST_DEFAULT_RECOVERY_RTT_MIN,
			.recovery_rtt_max = RIST_DEFAULT_RECOVERY_RTT_MAX,
			.weight = 5,
			.congestion_control_mode = RIST_DEFAULT_CONGESTION_CONTROL_MODE,
			.min_retries = RIST_DEFAULT_MIN_RETRIES,
			.max_retries = RIST_DEFAULT_MAX_RETRIES,
			.key_size = keysize,
		};

		if (setup->shared_secret != NULL) {
			strncpy(app_peer_config.secret, setup->shared_secret, RIST_MAX_STRING_SHORT - 1);
		}

		if (setup->cname != NULL) {
			strncpy(app_peer_config.cname, setup->cname, RIST_MAX_STRING_SHORT - 1);
		}

		// URL overrides (also cleans up the URL)
		struct rist_peer_config *peer_config = &app_peer_config;
		if (rist_parse_address2(setup->output_urls[i], &peer_config)) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse peer options for output URL: %s\n", setup->output_urls[i]);
			exit(1);
		}

		struct rist_peer *peer;
		if (rist_peer_create(ctx, &peer, peer_config) == -1) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not add peer connector for output URL: %s\n", setup->output_urls[i]);
			exit(1);
		}

		rist_log(&logging_settings, RIST_LOG_INFO, "Added output peer %d: %s\n", i + 1, setup->output_urls[i]);

#if HAVE_SRP_SUPPORT
		int srp_error = 0;
		if (strlen(peer_config->srp_username) > 0 && strlen(peer_config->srp_password) > 0) {
			srp_error = rist_enable_eap_srp_2(peer, peer_config->srp_username, peer_config->srp_password, NULL, NULL);
			if (srp_error)
				rist_log(&logging_settings, RIST_LOG_WARN, "Error %d trying to enable SRP for peer\n", srp_error);
		}
		if (srpfile) {
			srp_error = rist_enable_eap_srp_2(peer, NULL, NULL, user_verifier_lookup, srpfile);
			if (srp_error)
				rist_log(&logging_settings, RIST_LOG_WARN, "Error %d trying to enable SRP global authenticator, file %s\n", srp_error, srpfile);
		}
#endif
	}

	if (rist_start(ctx) == -1) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start rist sender\n");
		exit(1);
	}
	return ctx;
}

static int cb_recv(void *arg, struct rist_data_block *b)
{
	struct rist_cb_arg *cb_arg = (struct rist_cb_arg *) arg;
	struct rist_data_block *block = (struct rist_data_block*)b;

	// Handle SSRC passthrough
	if (cb_arg->client_args->ssrc_passthrough) {
		if (cb_arg->current_flow_id != b->flow_id) {
			rist_log(&logging_settings, RIST_LOG_INFO, "SSRC passthrough: Flow ID changed to %u\n", b->flow_id);
			cb_arg->current_flow_id = b->flow_id;
			cb_arg->client_args->flow_id = b->flow_id;
			assert(cb_arg->sender_ctx != NULL);
			rist_sender_flow_id_set(cb_arg->sender_ctx, b->flow_id);
		}
	}

	b->virt_src_port = cb_arg->src_port;
	b->virt_dst_port = cb_arg->dst_port;
	block->flags = RIST_DATA_FLAGS_USE_SEQ; // Preserve sequence numbers
	int ret = rist_sender_data_write(cb_arg->sender_ctx, b);
	rist_receiver_data_block_free2(&b);
	return ret;
}

static void intHandler(int signal) {
	rist_log(&logging_settings, RIST_LOG_NOTICE, "Signal %d received\n", signal);
	keep_running = 0;
}

int main(int argc, char **argv) {
	char *input_urls[MAX_INPUT_URLS] = {0};
	int input_url_count = 0;
	char *cname = NULL;
	struct rist_cb_arg cb_arg;
	struct rist_sender_args client_args;

	memset(&cb_arg, 0, sizeof(cb_arg));
	memset(&client_args, 0, sizeof(client_args));

	cb_arg.client_args = &client_args;
	cb_arg.src_port = 1971;
	cb_arg.dst_port = 1968;
	cb_arg.current_flow_id = 0;
	client_args.dst_port = 1968;
	client_args.encryption_type = 0;
	client_args.shared_secret = NULL;
	client_args.flow_id = 0;
	client_args.output_url_count = 0;
	client_args.npd_enabled = 0;
	client_args.ssrc_passthrough = 0;

	int statsinterval = 1000;
	enum rist_log_level loglevel = RIST_LOG_INFO;
	char *remote_log_address = NULL;
	int exitcode = 0;
	enum rist_profile profile = RIST_PROFILE_MAIN;

#ifdef _WIN32
#define STDERR_FILENO 2
	signal(SIGINT, intHandler);
	signal(SIGTERM, intHandler);
	signal(SIGABRT, intHandler);
#else
	struct sigaction act = { {0} };
	act.sa_handler = intHandler;
	act.sa_flags = 0;
	sigaction(SIGINT, &act, NULL);
#endif

	// Default log settings
	struct rist_logging_settings *log_ptr = &logging_settings;
	if (rist_logging_set(&log_ptr, loglevel, NULL, NULL, NULL, stderr) != 0) {
		fprintf(stderr, "Failed to setup default logging!\n");
		exit(1);
	}

	rist_log(&logging_settings, RIST_LOG_INFO, "Starting rist22rist version: %s libRIST library: %s API version: %s\n",
		RIST22RIST_VERSION, librist_version(), librist_api_version());

	int option_index;
	int c;
	while ((c = getopt_long(argc, argv, "r:i:o:s:e:N:v:S:p:nPMh:u", long_options, &option_index)) != -1) {
		switch (c) {
		case 'i':
			if (input_url_count >= MAX_INPUT_URLS) {
				rist_log(&logging_settings, RIST_LOG_ERROR, "Too many input URLs (max %d)\n", MAX_INPUT_URLS);
				exit(1);
			}
			input_urls[input_url_count++] = strdup(optarg);
			break;
		case 'o':
			if (client_args.output_url_count >= MAX_OUTPUT_URLS) {
				rist_log(&logging_settings, RIST_LOG_ERROR, "Too many output URLs (max %d)\n", MAX_OUTPUT_URLS);
				exit(1);
			}
			client_args.output_urls[client_args.output_url_count++] = strdup(optarg);
			break;
		case 's':
			if (client_args.shared_secret != NULL)
				goto usage;
			client_args.shared_secret = strdup(optarg);
			break;
		case 'e':
			client_args.encryption_type = atoi(optarg);
			break;
		case 'N':
			if (cname != NULL)
				goto usage;
			cname = strdup(optarg);
			break;
		case 'p':
			profile = atoi(optarg);
			break;
		case 'v':
			loglevel = (enum rist_log_level) atoi(optarg);
			break;
		case 'r':
			if (remote_log_address != NULL)
				goto usage;
			remote_log_address = strdup(optarg);
			break;
		case 'n':
			client_args.npd_enabled = 1;
			break;
		case 'P':
			client_args.ssrc_passthrough = 1;
			break;
#if HAVE_SRP_SUPPORT
		case 'F': {
			FILE* f = fopen(optarg, "r");
			if (!f) {
				rist_log(&logging_settings, RIST_LOG_ERROR, "Could not open srp file %s\n", optarg);
				return 1;
			}
			fclose(f);
			srpfile = strdup(optarg);
		}
		break;
#endif
#if HAVE_PROMETHEUS_SUPPORT
		case 'M':
			enable_prometheus = true;
			break;
		case 0:
			// Long option with flag pointer - already handled
			break;
		case 1:
			// --metrics-tags
			prometheus_tags = strdup(optarg);
			break;
		case 2:
			// --metrics-port
			prometheus_httpd = true;
			prometheus_port = atoi(optarg);
			break;
		case 3:
			// --metrics-ip
			prometheus_httpd = true;
			prometheus_ip = strdup(optarg);
			break;
#endif
		case 'S':
			statsinterval = atoi(optarg);
			break;
		case 'u':
			rist_log(&logging_settings, RIST_LOG_INFO, "%s", help_urlstr);
			exit(1);
		case 'h':
		default:
usage:
			usage(argv[0]);
			break;
		}
	}

	client_args.cname = cname;
	client_args.loglevel = loglevel;
	client_args.statsinterval = statsinterval;

	if (input_url_count == 0 || client_args.output_url_count == 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "At least one input (-i) and one output (-o) URL required\n");
		usage(argv[0]);
	}

	if (argc < 2) {
		usage(argv[0]);
	}

	if (rist_logging_set(&log_ptr, loglevel, NULL, NULL, remote_log_address, stderr) != 0) {
		fprintf(stderr, "Failed to setup logging!\n");
		exitcode = 1;
		goto out;
	}

	// Log configuration
	rist_log(&logging_settings, RIST_LOG_INFO, "Configuration:\n");
	rist_log(&logging_settings, RIST_LOG_INFO, "  Input peers: %d\n", input_url_count);
	for (int i = 0; i < input_url_count; i++) {
		rist_log(&logging_settings, RIST_LOG_INFO, "    [%d] %s\n", i + 1, input_urls[i]);
	}
	rist_log(&logging_settings, RIST_LOG_INFO, "  Output peers: %d\n", client_args.output_url_count);
	for (int i = 0; i < client_args.output_url_count; i++) {
		rist_log(&logging_settings, RIST_LOG_INFO, "    [%d] %s\n", i + 1, client_args.output_urls[i]);
	}
	rist_log(&logging_settings, RIST_LOG_INFO, "  NPD: %s\n", client_args.npd_enabled ? "enabled" : "disabled");
	rist_log(&logging_settings, RIST_LOG_INFO, "  SSRC passthrough: %s\n", client_args.ssrc_passthrough ? "enabled" : "disabled");

#if HAVE_PROMETHEUS_SUPPORT
	if (enable_prometheus || prometheus_httpd) {
		rist_log(log_ptr, RIST_LOG_INFO, "Enabling Metrics output\n");
		struct prometheus_httpd_options httpd_opt;
		httpd_opt.enabled = prometheus_httpd;
		httpd_opt.port = prometheus_port;
		httpd_opt.ip = prometheus_ip;
		httpd_opt.bind_sockaddr = false;
		prom_stats_ctx = rist_setup_prometheus_stats(log_ptr, prometheus_tags, false, false, &httpd_opt, NULL);
		if (prom_stats_ctx == NULL) {
			rist_log(log_ptr, RIST_LOG_ERROR, "Failed to setup Metrics output\n");
			exitcode = 1;
			goto out;
		}
		if (prometheus_httpd) {
			rist_log(log_ptr, RIST_LOG_INFO, "Metrics HTTP server listening on http://%s:%d/metrics\n",
				prometheus_ip ? prometheus_ip : "0.0.0.0", prometheus_port);
		}
	}
#endif

	// Create receiver context
	struct rist_ctx *receiver_ctx;

	if (rist_receiver_create(&receiver_ctx, profile, &logging_settings) != 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not create rist receiver context\n");
		exitcode = 1;
		goto out;
	}

	if (rist_auth_handler_set(receiver_ctx, cb_auth_connect, cb_auth_disconnect, receiver_ctx) == -1) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not init rist auth handler\n");
		exitcode = 1;
		goto out;
	}

	if (statsinterval) {
		rist_stats_callback_set(receiver_ctx, statsinterval, cb_stats, NULL);
	}

	// Add all input peers
	for (int i = 0; i < input_url_count; i++) {
		struct rist_peer_config app_peer_config = {
			.version = RIST_PEER_CONFIG_VERSION,
			.virt_dst_port = RIST_DEFAULT_VIRT_DST_PORT,
			.recovery_mode = RIST_DEFAULT_RECOVERY_MODE,
			.recovery_maxbitrate = RIST_DEFAULT_RECOVERY_MAXBITRATE,
			.recovery_maxbitrate_return = RIST_DEFAULT_RECOVERY_MAXBITRATE_RETURN,
			.recovery_length_min = RIST_DEFAULT_RECOVERY_LENGTH_MIN,
			.recovery_length_max = RIST_DEFAULT_RECOVERY_LENGTH_MAX,
			.recovery_reorder_buffer = RIST_DEFAULT_RECOVERY_REORDER_BUFFER,
			.recovery_rtt_min = RIST_DEFAULT_RECOVERY_RTT_MIN,
			.recovery_rtt_max = RIST_DEFAULT_RECOVERY_RTT_MAX,
			.weight = 5,
			.congestion_control_mode = RIST_DEFAULT_CONGESTION_CONTROL_MODE,
			.min_retries = RIST_DEFAULT_MIN_RETRIES,
			.max_retries = RIST_DEFAULT_MAX_RETRIES,
			.key_size = 0
		};

		if (cname != NULL) {
			strncpy(app_peer_config.cname, cname, RIST_MAX_STRING_SHORT - 1);
		}

		// URL overrides (also cleans up the URL)
		struct rist_peer_config *peer_config = &app_peer_config;
		if (rist_parse_address2(input_urls[i], &peer_config)) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse peer options for input URL: %s\n", input_urls[i]);
			exitcode = 1;
			goto out;
		}

		struct rist_peer *peer;
		if (rist_peer_create(receiver_ctx, &peer, peer_config) == -1) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not add peer connector for input URL: %s\n", input_urls[i]);
			exitcode = 1;
			goto out;
		}

		rist_log(&logging_settings, RIST_LOG_INFO, "Added input peer %d: %s\n", i + 1, input_urls[i]);
	}

	// Setup sender with all output peers
	cb_arg.sender_ctx = setup_rist_sender(&client_args);

	if (rist_start(receiver_ctx)) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start rist receiver\n");
		exitcode = 1;
		goto out;
	}

	rist_log(&logging_settings, RIST_LOG_INFO, "rist22rist gateway running...\n");

	// Master loop
	while (keep_running) {
		struct rist_data_block *b;
		int ret = rist_receiver_data_read2(receiver_ctx, &b, 5);
		if (ret && b && b->payload) cb_recv(&cb_arg, b);
	}

	rist_log(&logging_settings, RIST_LOG_INFO, "Shutting down...\n");
	rist_destroy(receiver_ctx);
	rist_destroy(cb_arg.sender_ctx);

out:
	rist_logging_unset_global();

	// Cleanup
	if (client_args.shared_secret)
		free(client_args.shared_secret);
	if (cname)
		free(cname);
	for (int i = 0; i < input_url_count; i++) {
		if (input_urls[i])
			free(input_urls[i]);
	}
	for (int i = 0; i < client_args.output_url_count; i++) {
		if (client_args.output_urls[i])
			free(client_args.output_urls[i]);
	}
	if (remote_log_address)
		free(remote_log_address);

#if HAVE_PROMETHEUS_SUPPORT
	rist_prometheus_stats_destroy(prom_stats_ctx);
	free(prometheus_ip);
	free(prometheus_tags);
#endif

	return exitcode;
}
