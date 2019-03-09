/*
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "opts.h"

#include <algorithm>                              // for std::max, std:min
#include <cmath>                                  // for std::ceil
#include <cstdio>                                 // for std::fprintf
#include <cstdlib>                                // for std::size_t, std::getenv, std::exit
#include <cstring>                                // for std::strchr, std::strrchr
#include <thread>                                 // for std::thread
#include <sysexits.h>                             // for EX_USAGE

#include "cmdoutput.h"                            // for CmdOutput
#include "ev/ev++.h"                              // for ev_supported
#include "hashes.hh"                              // for fnv1ah32
#include "string.hh"                              // for string::lower

#define XAPIAND_PID_FILE         "xapiand.pid"
#define XAPIAND_LOG_FILE         "xapiand.log"

#define FLUSH_THRESHOLD          100000           // Database flush threshold (default for xapian is 10000)
#define ENDPOINT_LIST_SIZE       10               // Endpoints List's size
#define NUM_REPLICAS             3                // Default number of database replicas per index

#define DBPOOL_SIZE              300              // Maximum number of database endpoints in database pool
#define MAX_DATABASES            400              // Maximum number of open databases
#define MAX_CLIENTS              1000             // Maximum number of open client connections

#define NUM_HTTP_SERVERS             2.0          // Number of servers per CPU
#define MAX_HTTP_SERVERS              16

#define NUM_HTTP_CLIENTS            16.0          // Number of http client threads per CPU
#define MAX_HTTP_CLIENTS             128

#define NUM_REMOTE_SERVERS           2.0          // Number of remote protocol client threads per CPU
#define MAX_REMOTE_SERVERS            16

#define NUM_REMOTE_CLIENTS          16.0          // Number of remote protocol client threads per CPU
#define MAX_REMOTE_CLIENTS           128

#define NUM_REPLICATION_SERVERS      2.0          // Number of replication protocol client threads per CPU
#define MAX_REPLICATION_SERVERS       16

#define NUM_REPLICATION_CLIENTS     16.0          // Number of replication protocol client threads per CPU
#define MAX_REPLICATION_CLIENTS      128

#define NUM_ASYNC_WAL_WRITERS        1.0          // Number of database async WAL writers per CPU
#define MAX_ASYNC_WAL_WRITERS          8

#define NUM_DOC_PREPARERS            8.0          // Number of threads handling bulk documents preparing per CPU
#define MAX_DOC_PREPARERS             64

#define NUM_DOC_INDEXERS             2.0          // Number of threads handling bulk documents indexing per CPU
#define MAX_DOC_INDEXERS              16

#define NUM_COMMITTERS               0.5          // Number of threads handling the commits per CPU
#define MAX_COMMITTERS                 8

#define NUM_FSYNCHERS                0.5          // Number of threads handling the fsyncs per CPU
#define MAX_FSYNCHERS                  8

#define NUM_REPLICATORS              0.5          // Number of threads handling the replication per CPU
#define MAX_REPLICATORS                4

#define NUM_DISCOVERERS              0.5          // Number of threads handling the discoverers per CPU
#define MAX_DISCOVERERS                4


#define EV_SELECT_NAME  "select"
#define EV_POLL_NAME    "poll"
#define EV_EPOLL_NAME   "epoll"
#define EV_KQUEUE_NAME  "kqueue"
#define EV_DEVPOLL_NAME "devpoll"
#define EV_PORT_NAME    "port"


unsigned int
ev_backend(const std::string& name)
{
	auto ev_use = string::lower(name);
	if (ev_use.empty() || ev_use.compare("auto") == 0) {
		return ev::AUTO;
	}
	if (ev_use.compare(EV_SELECT_NAME) == 0) {
		return ev::SELECT;
	}
	if (ev_use.compare(EV_POLL_NAME) == 0) {
		return ev::POLL;
	}
	if (ev_use.compare(EV_EPOLL_NAME) == 0) {
		return ev::EPOLL;
	}
	if (ev_use.compare(EV_KQUEUE_NAME) == 0) {
		return ev::KQUEUE;
	}
	if (ev_use.compare(EV_DEVPOLL_NAME) == 0) {
		return ev::DEVPOLL;
	}
	if (ev_use.compare(EV_PORT_NAME) == 0) {
		return ev::PORT;
	}
	return -1;
}


const char*
ev_backend(unsigned int backend)
{
	switch(backend) {
		case ev::SELECT:
			return EV_SELECT_NAME;
		case ev::POLL:
			return EV_POLL_NAME;
		case ev::EPOLL:
			return EV_EPOLL_NAME;
		case ev::KQUEUE:
			return EV_KQUEUE_NAME;
		case ev::DEVPOLL:
			return EV_DEVPOLL_NAME;
		case ev::PORT:
			return EV_PORT_NAME;
	}
	return "unknown";
}


std::vector<std::string> ev_supported() {
	std::vector<std::string> backends;
	unsigned int supported = ev::supported_backends();
	if ((supported & ev::SELECT) != 0u) { backends.emplace_back(EV_SELECT_NAME); }
	if ((supported & ev::POLL) != 0u) { backends.emplace_back(EV_POLL_NAME); }
	if ((supported & ev::EPOLL) != 0u) { backends.emplace_back(EV_EPOLL_NAME); }
	if ((supported & ev::KQUEUE) != 0u) { backends.emplace_back(EV_KQUEUE_NAME); }
	if ((supported & ev::DEVPOLL) != 0u) { backends.emplace_back(EV_DEVPOLL_NAME); }
	if ((supported & ev::PORT) != 0u) { backends.emplace_back(EV_PORT_NAME); }
	if (backends.empty()) {
		backends.emplace_back("auto");
	}
	return backends;
}


opts_t
parseOptions(int argc, char** argv)
{
	opts_t o;

	const double hardware_concurrency = std::thread::hardware_concurrency();

	using namespace TCLAP;

	try {
		CmdLine cmd("", ' ', Package::VERSION);

		CmdOutput output;
		ZshCompletionOutput zshoutput;

		if (std::getenv("ZSH_COMPLETE") != nullptr) {
			cmd.setOutput(&zshoutput);
		} else {
			cmd.setOutput(&output);
		}

#ifdef XAPIAND_RANDOM_ERRORS
		ValueArg<double> random_errors_net("", "random-errors-net", "Inject random network errors with this probability (0-1)", false, 0, "probability", cmd);
		ValueArg<double> random_errors_io("", "random-errors-io", "Inject random IO errors with this probability (0-1)", false, 0, "probability", cmd);
		ValueArg<double> random_errors_db("", "random-errors-db", "Inject random database errors with this probability (0-1)", false, 0, "probability", cmd);
#endif

		ValueArg<std::string> out("o", "out", "Output filename for dump.", false, "", "file", cmd);
		ValueArg<std::string> dump_metadata("", "dump-metadata", "Dump endpoint metadata to stdout.", false, "", "endpoint", cmd);
		ValueArg<std::string> dump_schema("", "dump-schema", "Dump endpoint schema to stdout.", false, "", "endpoint", cmd);
		ValueArg<std::string> dump_documents("", "dump", "Dump endpoint to stdout.", false, "", "endpoint", cmd);
		ValueArg<std::string> in("i", "in", "Input filename for restore.", false, "", "file", cmd);
		ValueArg<std::string> restore("", "restore", "Restore endpoint from stdin.", false, "", "endpoint", cmd);

		MultiSwitchArg verbose("v", "verbose", "Increase verbosity.", cmd);
		ValueArg<unsigned int> verbosity("", "verbosity", "Set verbosity.", false, 0, "verbosity", cmd);

#ifdef XAPIAN_HAS_GLASS_BACKEND
		SwitchArg chert("", "chert", "Prefer Chert databases.", cmd, false);
#endif

		std::vector<std::string> uuid_allowed({
			"vanilla",
#ifdef XAPIAND_UUID_GUID
			"guid",
#endif
#ifdef XAPIAND_UUID_URN
			"urn",
#endif
#ifdef XAPIAND_UUID_ENCODED
			"compact",
			"encoded",
			"partition",
#endif
		});
		ValuesConstraint<std::string> uuid_constraint(uuid_allowed);
		MultiArg<std::string> uuid("", "uuid", "Toggle modes for compact and/or encoded UUIDs and UUID index path partitioning.", false, &uuid_constraint, cmd);

#ifdef XAPIAND_CLUSTERING
		ValueArg<unsigned int> discovery_port("", "discovery-port", "Discovery UDP port number to listen on.", false, 0, "port", cmd);
		ValueArg<std::string> discovery_group("", "discovery-group", "Discovery UDP group name.", false, XAPIAND_DISCOVERY_GROUP, "group", cmd);
		ValueArg<std::string> cluster_name("", "cluster", "Cluster name to join.", false, XAPIAND_CLUSTER_NAME, "cluster", cmd);
		ValueArg<std::string> node_name("", "name", "Node name.", false, "", "node", cmd);
#endif

#if XAPIAND_DATABASE_WAL
		ValueArg<std::size_t> num_async_wal_writers("", "writers", "Number of database async wal writers.", false, 0, "writers", cmd);
#endif
#ifdef XAPIAND_CLUSTERING
		ValueArg<std::size_t> num_replicas("", "replicas", "Default number of database replicas per index.", false, NUM_REPLICAS, "replicas", cmd);
#endif
		ValueArg<std::size_t> num_doc_preparers("", "bulk-preparers", "Number of threads handling bulk documents preparing.", false, 0, "threads", cmd);
		ValueArg<std::size_t> num_doc_indexers("", "bulk-indexers", "Number of threads handling bulk documents indexing.", false, 0, "threads", cmd);
		ValueArg<std::size_t> num_committers("", "committers", "Number of threads handling the commits.", false, 0, "threads", cmd);
		ValueArg<std::size_t> max_databases("", "max-databases", "Max number of open databases.", false, MAX_DATABASES, "databases", cmd);
		ValueArg<std::size_t> dbpool_size("", "dbpool-size", "Maximum number of databases in database pool.", false, DBPOOL_SIZE, "size", cmd);

		ValueArg<std::size_t> num_fsynchers("", "fsynchers", "Number of threads handling the fsyncs.", false, 0, "fsynchers", cmd);
#ifdef XAPIAND_CLUSTERING
		ValueArg<std::size_t> num_replicators("", "replicators", "Number of replicators triggering database replication.", false, 0, "replicators", cmd);
		ValueArg<std::size_t> num_discoverers("", "discoverers", "Number of discoverers doing cluster discovery.", false, 0, "discoverers", cmd);
#endif

		ValueArg<std::size_t> max_files("", "max-files", "Maximum number of files to open.", false, 0, "files", cmd);
		ValueArg<std::size_t> flush_threshold("", "flush-threshold", "Xapian flush threshold.", false, FLUSH_THRESHOLD, "threshold", cmd);

#ifdef XAPIAND_CLUSTERING
		ValueArg<std::size_t> num_remote_clients("", "remote-clients", "Number of remote protocol client threads.", false, 0, "threads", cmd);
		ValueArg<std::size_t> num_remote_servers("", "remote-servers", "Number of remote protocol servers.", false, 0, "servers", cmd);
		ValueArg<std::size_t> num_replication_clients("", "replication-clients", "Number of replication protocol client threads.", false, 0, "threads", cmd);
		ValueArg<std::size_t> num_replication_servers("", "replication-servers", "Number of replication protocol servers.", false, 0, "servers", cmd);
#endif
		ValueArg<std::size_t> num_http_clients("", "http-clients", "Number of http client threads.", false, 0, "threads", cmd);
		ValueArg<std::size_t> num_http_servers("", "http-servers", "Number of http servers.", false, 0, "servers", cmd);
		ValueArg<std::size_t> max_clients("", "max-clients", "Max number of open client connections.", false, MAX_CLIENTS, "clients", cmd);

		ValueArg<double> processors("", "processors", "Number of processors to use.", false, 1, "processors", cmd);

		auto use_allowed = ev_supported();
		ValuesConstraint<std::string> use_constraint(use_allowed);
		ValueArg<std::string> use("", "use", "Connection processing backend.", false, "auto", &use_constraint, cmd);

#ifdef XAPIAND_CLUSTERING
		ValueArg<unsigned int> remote_port("", "xapian-port", "Xapian binary protocol TCP port number to listen on.", false, 0, "port", cmd);
		ValueArg<unsigned int> replication_port("", "replica-port", "Xapiand replication protocol TCP port number to listen on.", false, 0, "port", cmd);
#endif
		ValueArg<unsigned int> http_port("", "port", "TCP HTTP port number to listen on for REST API.", false, 0, "port", cmd);

		ValueArg<std::string> bind_address("", "bind-address", "Bind address to listen to.", false, "", "bind", cmd);

		SwitchArg iterm2("", "iterm2", "Set marks, tabs, title, badges and growl.", cmd, false);

		SwitchArg log_epoch("", "log-epoch", "Logs timestamp as epoch time.", cmd, false);
		SwitchArg log_iso8601("", "log-iso8601", "Logs timestamp as iso8601.", cmd, false);
		SwitchArg log_timeless("", "log-timeless", "Logs without timestamp.", cmd, false);
		SwitchArg log_plainseconds("", "log-seconds", "Log timestamps with plain seconds.", cmd, false);
		SwitchArg log_milliseconds("", "log-milliseconds", "Log timestamps with milliseconds.", cmd, false);
		SwitchArg log_microseconds("", "log-microseconds", "Log timestamps with microseconds.", cmd, false);
		SwitchArg log_threads("", "log-threads", "Logs thread names.", cmd, false);
#ifndef NDEBUG
		SwitchArg log_location("", "log-location", "Logs log location.", cmd, false);
#endif
		ValueArg<std::string> gid("", "gid", "Group ID.", false, "", "gid", cmd);
		ValueArg<std::string> uid("", "uid", "User ID.", false, "", "uid", cmd);

		ValueArg<std::string> pidfile("P", "pidfile", "Save PID in <file>.", false, "", "file", cmd);
		ValueArg<std::string> logfile("L", "logfile", "Save logs in <file>.", false, "", "file", cmd);

		SwitchArg admin_commands("", "admin-commands", "Enables administrative HTTP commands.", cmd, false);

		SwitchArg no_colors("", "no-colors", "Disables colors on the console.", cmd, false);
		SwitchArg colors("", "colors", "Enables colors on the console.", cmd, false);

		SwitchArg detach("d", "detach", "detach process. (run in background)", cmd);
#ifdef XAPIAND_CLUSTERING
		SwitchArg solo("", "solo", "Run solo indexer. (no replication or discovery)", cmd, false);
#endif
		SwitchArg foreign("", "foreign", "Force foreign (shared) schemas for all indexes.", cmd, false);
		SwitchArg strict("", "strict", "Force the user to define the type for each field.", cmd, false);
		SwitchArg force("", "force", "Force using path as the root of the node.", cmd, false);
		ValueArg<std::string> database("D", "database", "Path to the root of the node.", false, XAPIAND_ROOT "/var/db/xapiand", "path", cmd);

		std::vector<std::string> args;
		for (int i = 0; i < argc; ++i) {
			if (i == 0) {
				const char* a = std::strrchr(argv[i], '/');
				if (a != nullptr) {
					++a;
				} else {
					a = argv[i];
				}
				args.emplace_back(a);
			} else {
				// Split arguments when possible (e.g. -Dnode, --verbosity=3)
				const char* arg = argv[i];
				if (arg[0] == '-') {
					if (arg[1] != '-' && arg[1] != 'v') {  // skip long arguments (e.g. --verbosity) or multiswitch (e.g. -vvv)
						std::string tmp(arg, 2);
						args.push_back(tmp);
						arg += 2;
					}
				}
				const char* a = std::strchr(arg, '=');
				if (a != nullptr) {
					if ((a - arg) != 0) {
						std::string tmp(arg, a - arg);
						args.push_back(tmp);
					}
					arg = a + 1;
				}
				if (*arg != 0) {
					args.emplace_back(arg);
				}
			}
		}

		cmd.parse(args);

#ifdef XAPIAND_RANDOM_ERRORS
		o.random_errors_db = random_errors_db.getValue();
		o.random_errors_io = random_errors_io.getValue();
		o.random_errors_net = random_errors_net.getValue();
#endif

		o.processors = std::max(1.0, std::min(processors.getValue(), hardware_concurrency));
		o.verbosity = verbosity.getValue() + verbose.getValue();
		o.detach = detach.getValue();
#ifdef XAPIAN_HAS_GLASS_BACKEND
		o.chert = chert.getValue();
#else
		o.chert = true;
#endif

#ifdef XAPIAND_CLUSTERING
		o.solo = solo.getValue();
#else
		o.solo = true;
#endif
		o.strict = strict.getValue();
		o.foreign = foreign.getValue();
		o.force = force.getValue();

		o.colors = colors.getValue();
		o.no_colors = no_colors.getValue();

		o.admin_commands = admin_commands.getValue();

		o.iterm2 = iterm2.getValue();

		o.log_epoch = log_epoch.getValue();
		o.log_iso8601 = log_iso8601.getValue();
		o.log_timeless = log_timeless.getValue();
		o.log_plainseconds = log_plainseconds.getValue();
		o.log_milliseconds = log_milliseconds.getValue();
		o.log_microseconds = log_microseconds.getValue();
#ifndef NDEBUG
		if (!o.log_plainseconds && !o.log_milliseconds && !o.log_microseconds) {
			o.log_microseconds = true;
		}
#endif

		o.log_threads = log_threads.getValue();

#ifndef NDEBUG
		o.log_location = log_location.getValue();
#endif

		o.database = database.getValue();
		if (o.database.empty()) {
			o.database = ".";
		}
		o.http_port = http_port.getValue();
		o.bind_address = bind_address.getValue();
#ifdef XAPIAND_CLUSTERING
		o.cluster_name = cluster_name.getValue();
		o.node_name = node_name.getValue();
		o.remote_port = remote_port.getValue();
		o.replication_port = replication_port.getValue();
		o.discovery_port = discovery_port.getValue();
		o.discovery_group = discovery_group.getValue();
#endif
		o.pidfile = pidfile.getValue();
		o.logfile = logfile.getValue();
		o.uid = uid.getValue();
		o.gid = gid.getValue();
		o.dbpool_size = dbpool_size.getValue();
#if XAPIAND_DATABASE_WAL
		o.num_async_wal_writers = num_async_wal_writers.getValue() || std::min(MAX_ASYNC_WAL_WRITERS, static_cast<int>(std::ceil(NUM_ASYNC_WAL_WRITERS * o.processors)));
#endif
#ifdef XAPIAND_CLUSTERING
		o.num_replicas = o.solo ? 0 : num_replicas.getValue();
#endif
		o.num_doc_preparers = num_doc_preparers.getValue() || std::min(MAX_DOC_PREPARERS, static_cast<int>(std::ceil(NUM_DOC_PREPARERS * o.processors)));
		o.num_doc_indexers = num_doc_indexers.getValue() || std::min(MAX_DOC_INDEXERS, static_cast<int>(std::ceil(NUM_DOC_INDEXERS * o.processors)));
		o.num_committers = num_committers.getValue() || std::min(MAX_COMMITTERS, static_cast<int>(std::ceil(NUM_COMMITTERS * o.processors)));
		o.num_fsynchers = num_fsynchers.getValue() || std::min(MAX_FSYNCHERS, static_cast<int>(std::ceil(NUM_FSYNCHERS * o.processors)));
		o.num_replicators = num_replicators.getValue() || std::min(MAX_REPLICATORS, static_cast<int>(std::ceil(NUM_REPLICATORS * o.processors)));
		o.num_discoverers = num_discoverers.getValue() || std::min(MAX_DISCOVERERS, static_cast<int>(std::ceil(NUM_DISCOVERERS * o.processors)));

		o.max_clients = max_clients.getValue();
		o.max_databases = max_databases.getValue();
		o.max_files = max_files.getValue();
		o.flush_threshold = flush_threshold.getValue();
		o.num_http_clients = num_http_clients.getValue() || std::min(MAX_HTTP_CLIENTS, static_cast<int>(std::ceil(NUM_HTTP_CLIENTS * o.processors)));
		o.num_http_servers = num_http_servers.getValue() || std::min(MAX_HTTP_SERVERS, static_cast<int>(std::ceil(NUM_HTTP_SERVERS * o.processors)));
#ifdef XAPIAND_CLUSTERING
		o.num_remote_clients = num_remote_clients.getValue() || std::min(MAX_REMOTE_CLIENTS, static_cast<int>(std::ceil(NUM_REMOTE_CLIENTS * o.processors)));
		o.num_remote_servers = num_remote_servers.getValue() || std::min(MAX_REMOTE_SERVERS, static_cast<int>(std::ceil(NUM_REMOTE_SERVERS * o.processors)));
		o.num_replication_clients = num_replication_clients.getValue() || std::min(MAX_REPLICATION_CLIENTS, static_cast<int>(std::ceil(NUM_REPLICATION_CLIENTS * o.processors)));
		o.num_replication_servers = num_replication_servers.getValue() || std::min(MAX_REPLICATION_SERVERS, static_cast<int>(std::ceil(NUM_REPLICATION_SERVERS * o.processors)));
#endif
		o.endpoints_list_size = ENDPOINT_LIST_SIZE;
		if (o.detach) {
			if (o.logfile.empty()) {
				o.logfile = XAPIAND_ROOT "/var/log/" XAPIAND_LOG_FILE;
			}
			if (o.pidfile.empty()) {
				o.pidfile = XAPIAND_ROOT "/var/run/" XAPIAND_PID_FILE;
			}
		}
		o.ev_flags = ev_backend(use.getValue());

		bool uuid_configured = false;
		for (const auto& u : uuid.getValue()) {
			switch (fnv1ah32::hash(u)) {
				case fnv1ah32::hash("vanilla"):
					o.uuid_repr = fnv1ah32::hash("vanilla");
					uuid_configured = true;
					break;
#ifdef XAPIAND_UUID_GUID
				case fnv1ah32::hash("guid"):
					o.uuid_repr = fnv1ah32::hash("guid");
					uuid_configured = true;
					break;
#endif
#ifdef XAPIAND_UUID_URN
				case fnv1ah32::hash("urn"):
					o.uuid_repr = fnv1ah32::hash("urn");
					uuid_configured = true;
					break;
#endif
#ifdef XAPIAND_UUID_ENCODED
				case fnv1ah32::hash("encoded"):
					o.uuid_repr = fnv1ah32::hash("encoded");
					uuid_configured = true;
					break;
#endif
				case fnv1ah32::hash("compact"):
					o.uuid_compact = true;
					break;
				case fnv1ah32::hash("partition"):
					o.uuid_partition = true;
					break;
			}
		}
		if (!uuid_configured) {
#ifdef XAPIAND_UUID_ENCODED
			o.uuid_repr = fnv1ah32::hash("encoded");
#else
			o.uuid_repr = fnv1ah32::hash("vanilla");
#endif
			o.uuid_compact = true;
		}

		o.dump_metadata = dump_metadata.getValue();
		o.dump_schema = dump_schema.getValue();
		o.dump_documents = dump_documents.getValue();
		auto out_filename = out.getValue();
		o.restore = restore.getValue();
		auto in_filename = in.getValue();
		if ((!o.dump_metadata.empty() || !o.dump_schema.empty() || !o.dump_documents.empty()) && !o.restore.empty()) {
			throw CmdLineParseException("Cannot dump and restore at the same time");
		} else if (!o.dump_metadata.empty() || !o.dump_schema.empty() || !o.dump_documents.empty() || !o.restore.empty()) {
			if (!o.restore.empty()) {
				if (!out_filename.empty()) {
					throw CmdLineParseException("Option invalid: --out <file> can be used only with --dump");
				}
				o.filename = in_filename;
			} else {
				if (!in_filename.empty()) {
					throw CmdLineParseException("Option invalid: --in <file> can be used only with --restore");
				}
				o.filename = out_filename;
			}
			o.detach = false;
		} else {
			if (!in_filename.empty()) {
				throw CmdLineParseException("Option invalid: --in <file> can be used only with --restore");
			}
			if (!out_filename.empty()) {
				throw CmdLineParseException("Option invalid: --out <file> can be used only with --dump");
			}
		}

	} catch (const ArgException& exc) { // catch any exceptions
		std::fprintf(stderr, "Error: %s for arg %s\n", exc.error().c_str(), exc.argId().c_str());
		std::exit(EX_USAGE);
	}

	return o;
}