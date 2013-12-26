#include "main.h"
#ifndef _WIN32
#include <execinfo.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include <fstream>

#include "configuration.h"
#include "connection_manager.h"
#include "blocking_queue.h"
#include "sender.h"
#include "receiver.h"
#include "error_handler.h"
#include "sinsp_data_handler.h"

#define AGENT_PRIORITY 19
#define SOCKETBUFFER_STORAGE_SIZE (2 * 1024 * 1024)

dragent_logger* g_log = NULL;

static void g_monitor_signal_callback(int sig)
{
	exit(EXIT_SUCCESS);
}

static void g_signal_callback(int sig)
{
	dragent_configuration::m_terminate = true;
}

static void g_usr_signal_callback(int sig)
{
	g_log->information("Received SIGUSR1, toggling capture state"); 
	dragent_configuration::m_dump_enabled = !dragent_configuration::m_dump_enabled;
}

#ifndef _WIN32
static const int g_crash_signals[] = 
{
	SIGSEGV,
	SIGABRT,
	SIGFPE,
	SIGILL,
	SIGBUS
};

static void g_crash_handler(int sig)
{
	static int NUM_FRAMES = 10;

	if(g_log)
	{
		g_log->error("Received signal " + NumberFormatter::format(sig));

		void *array[NUM_FRAMES];

		int frames = backtrace(array, NUM_FRAMES);
		
		char **strings = backtrace_symbols(array, frames);
		
		if(strings != NULL)
		{
			for(int32_t j = 0; j < frames; ++j)
			{
				g_log->error(strings[j]);
			}

			free(strings);
		}
	}

	signal(sig, SIG_DFL);
	raise(sig);
}

static bool initialize_crash_handler()
{
	stack_t stack;

	memset(&stack, 0, sizeof(stack));
	stack.ss_sp = malloc(SIGSTKSZ);
	stack.ss_size = SIGSTKSZ;

	if(sigaltstack(&stack, NULL) == -1)
	{
		free(stack.ss_sp);
		return false;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);

	for(uint32_t j = 0; j < sizeof(g_crash_signals) / sizeof(g_crash_signals[0]); ++j)
	{
		sigaddset(&sa.sa_mask, g_crash_signals[j]);
	}

	sa.sa_handler = g_crash_handler;
	sa.sa_flags = SA_ONSTACK;

	for(uint32_t j = 0; j < sizeof(g_crash_signals) / sizeof(g_crash_signals[0]); ++j)
	{
		if(sigaction(g_crash_signals[j], &sa, NULL) != 0)
		{
			return false;
		}
	}

	return true;
}

static void run_monitor(const string& pidfile)
{
	signal(SIGINT, g_monitor_signal_callback);
	signal(SIGTERM, g_monitor_signal_callback);
	signal(SIGUSR1, SIG_IGN);

	//
	// Start the monitor process
	// 
	int result = fork();

	if(result < 0)
	{
		exit(EXIT_FAILURE);
	}

	if(result)
	{
		//
		// Father. It will be the monitor process
		//
		while(true)
		{
			int status = 0;

			wait(&status);

			if(!WIFEXITED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0))
			{
				//
				// Since both child and father are run with --daemon option,
				// Poco can get confused and can delete the pidfile even if
				// the monitor doesn't die.
				//
				if(!pidfile.empty())
				{
					std::ofstream ostr(pidfile);
					if(ostr.good())
					{
						ostr << Poco::Process::id() << std::endl;
					}
				}

				//
				// Sleep for a bit and run another dragent
				//
				sleep(1);

				result = fork();
				if(result == 0)
				{
					break;
				}

				if(result < 0)
				{
					exit(EXIT_FAILURE);
				}
			}
			else
			{
				exit(EXIT_SUCCESS);
			}
		}
	}

	//
	// We want to terminate when the monitor is killed by init
	//
	prctl(PR_SET_PDEATHSIG, SIGTERM);
}
#endif

///////////////////////////////////////////////////////////////////////////////
// Log management
///////////////////////////////////////////////////////////////////////////////

void g_logger_callback(char* str, uint32_t sev)
{
	ASSERT(g_log != NULL);

	switch(sev)
	{
	case sinsp_logger::SEV_DEBUG:
		g_log->debug(str);
		break;
	case sinsp_logger::SEV_INFO:
		g_log->information(str);
		break;
	case sinsp_logger::SEV_WARNING:
		g_log->warning(str);
		break;
	case sinsp_logger::SEV_ERROR:
		g_log->error(str);
		break;
	case sinsp_logger::SEV_CRITICAL:
		g_log->critical(str);
		break;
	default:
		ASSERT(false);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Capture information class
///////////////////////////////////////////////////////////////////////////////
class captureinfo
{
public:
	captureinfo()
	{
		m_nevts = 0;
		m_time = 0;
	}

	uint64_t m_nevts;
	uint64_t m_time;
};

///////////////////////////////////////////////////////////////////////////////
// The main application class
///////////////////////////////////////////////////////////////////////////////
class dragent_app: public Poco::Util::ServerApplication
{
public:
	dragent_app(): m_help_requested(false)
	{
		m_evtcnt = 0;

	}
	
	~dragent_app()
	{
		if(g_log != NULL)
		{
			delete g_log;
		}
	}

protected:
	void initialize(Application& self)
	{
		ServerApplication::initialize(self);
	}
		
	void uninitialize()
	{
		ServerApplication::uninitialize();
	}

	void defineOptions(OptionSet& options)
	{
		ServerApplication::defineOptions(options);
		
		options.addOption(
			Option("help", "h", "display help information on command line arguments")
				.required(false)
				.repeatable(false));

		options.addOption(
			Option("consolepriority", "", "min priority of the log messages that go on console. Can be 'error', 'warning', 'info' or 'debug'.")
				.required(false)
				.repeatable(false)
				.argument("priority"));

		options.addOption(
			Option("filepriority", "", "min priority of the log messages that go on file. Can be 'error', 'warning', 'info' or 'debug'.")
				.required(false)
				.repeatable(false)
				.argument("priority"));

		options.addOption(
			Option("readfile", "r", "file to open.")
				.required(false)
				.repeatable(false)
				.argument("filename"));

		options.addOption(
			Option("evtcount", "c", "numer of events after which the capture stops.")
				.required(false)
				.repeatable(false)
				.argument("count"));

		options.addOption(
			Option("customerid", "i", "force the customer id.")
				.required(false)
				.repeatable(false)
				.argument("id"));

		options.addOption(
			Option("writefile", "w", "specify this flag to save all the capture events to the 'filename' file.")
				.required(false)
				.repeatable(false)
				.argument("filename"));

		options.addOption(
			Option("srvaddr", "", "the address of the server to connect to.")
				.required(false)
				.repeatable(false)
				.argument("address"));

		options.addOption(
			Option("srvport", "", "the TCP port to use.")
				.required(false)
				.repeatable(false)
				.argument("port"));
	}

	void handleOption(const std::string& name, const std::string& value)
	{
		ServerApplication::handleOption(name, value);

		if(name == "help")
		{
			m_help_requested = true;
		}
		else if(name == "consolepriority")
		{
			m_configuration.m_min_console_priority = dragent_configuration::string_to_priority(value);
		}
		else if(name == "filepriority")
		{
			m_configuration.m_min_file_priority = dragent_configuration::string_to_priority(value);
		}
		else if(name == "readfile")
		{
			m_filename = value;
		}
		else if(name == "evtcount")
		{
			m_evtcnt = NumberParser::parse64(value);
		}
		else if(name == "customerid")
		{
			m_configuration.m_customer_id = value;
		}
		else if(name == "writefile")
		{
			m_writefile = value;
		}
		else if(name == "srvaddr")
		{
			m_configuration.m_server_addr = value;
		}
		else if(name == "srvport")
		{
			m_configuration.m_server_port = (uint16_t)NumberParser::parse(value);
		}
		else if(name == "pidfile")
		{
			m_pidfile = value;
		}
	}

	void displayHelp()
	{
		HelpFormatter helpFormatter(options());
		helpFormatter.setCommand(commandName());
		helpFormatter.setUsage("OPTIONS");
		helpFormatter.setHeader("Draios Agent.");
		helpFormatter.format(std::cout);
	}

	///////////////////////////////////////////////////////////////////////////
	// Event processing loop.
	// We don't do much other than consuming the events and updating a couple
	// of counters.
	///////////////////////////////////////////////////////////////////////////
	captureinfo do_inspect()
	{
		captureinfo retval;
		int32_t res;
		sinsp_evt* ev;
		uint64_t ts;
		uint64_t deltats = 0;
		uint64_t firstts = 0;
		bool capturing = false;

		while(1)
		{
			if((m_evtcnt != 0 && retval.m_nevts == m_evtcnt) || 
				dragent_configuration::m_terminate || 
				dragent_error_handler::m_exception)
			{
				break;
			}

			if(capturing)
			{
				if(!dragent_configuration::m_dump_enabled)
				{
					g_log->information("Stopping dump");
					capturing = false;
					m_inspector.stop_dump();
					m_configuration.m_dump_completed.set();
				}
			}
			else
			{
				if(dragent_configuration::m_dump_enabled)
				{
					g_log->information("Starting dump");
					capturing = true;
					m_configuration.m_dump_completed.reset();
					m_inspector.start_dump(m_configuration.m_dump_file);
				}
			}

			res = m_inspector.next(&ev);

			if(res == SCAP_TIMEOUT)
			{
				continue;
			}
			else if(res == SCAP_EOF)
			{
				break;
			}
			else if(res != SCAP_SUCCESS)
			{
				cerr << "res = " << res << endl;
				throw sinsp_exception(m_inspector.getlasterr().c_str());
			}

			//
			// Update the event count
			//
			retval.m_nevts++;

			//
			// Update the time 
			//
			ts = ev->get_ts();

			if(firstts == 0)
			{
				firstts = ts;
			}

			deltats = ts - firstts;
		}

		retval.m_time = deltats;
		return retval;
	}

	///////////////////////////////////////////////////////////////////////////
	// MAIN
	///////////////////////////////////////////////////////////////////////////
	int main(const std::vector<std::string>& args)
	{
		if(m_help_requested)
		{
			displayHelp();
			return Application::EXIT_OK;
		}

#ifndef _WIN32
		if(config().getBool("application.runAsDaemon", false))
		{
			run_monitor(m_pidfile);
		}

		if(signal(SIGINT, g_signal_callback) == SIG_ERR)
		{
			ASSERT(false);
		}

		if(signal(SIGTERM, g_signal_callback) == SIG_ERR)
		{
			ASSERT(false);
		}

		if(signal(SIGUSR1, g_usr_signal_callback) == SIG_ERR)
		{
			ASSERT(false);
		}

		if(initialize_crash_handler() == false)
		{
			ASSERT(false);
		}
#endif

		dragent_error_handler error_handler;
		Poco::ErrorHandler::set(&error_handler);

		m_configuration.init(this);

		//
		// Create the logs directory if it doesn't exist
		//
		File d(m_configuration.m_log_dir);
		d.createDirectories();
		Path p;
		p.parseDirectory(m_configuration.m_log_dir);
		p.setFileName("draios.log");
		string logsdir = p.toString();

		//
		// Setup the logging
		//
		AutoPtr<Channel> console_channel(new ConsoleChannel());
		AutoPtr<FileChannel> file_channel(new FileChannel(logsdir));

		file_channel->setProperty("rotation", "10M");
		file_channel->setProperty("purgeCount", "5");
		file_channel->setProperty("archive", "timestamp");

		AutoPtr<Formatter> formatter(new PatternFormatter("%h-%M-%S.%i, %p, %t"));

		AutoPtr<Channel> formatting_channel_file(new FormattingChannel(formatter, file_channel));
		AutoPtr<Channel> formatting_channel_console(new FormattingChannel(formatter, console_channel));

		Logger& loggerf = Logger::create("DraiosLogF", formatting_channel_file, m_configuration.m_min_file_priority);
		Logger& loggerc = Logger::create("DraiosLogC", formatting_channel_console, m_configuration.m_min_console_priority);
		
		if(m_configuration.m_min_console_priority != -1)
		{
			g_log = new dragent_logger(&loggerf, &loggerc);
		}
		else
		{
			g_log = new dragent_logger(&loggerf, NULL);
		}

		g_log->information("Agent starting");

		m_configuration.print_configuration();

		connection_manager m_connection_manager;
		dragent_queue queue;
		sinsp_data_handler sinsp_handler(&queue, &m_configuration);
		dragent_sender sender_thread(&queue, &m_connection_manager);
		dragent_receiver receiver_thread(&queue, &m_configuration, &m_connection_manager);

#if 0
		if(m_configuration.m_daemon)
		{
#ifndef _WIN32
			if(nice(AGENT_PRIORITY) == -1)
			{
				ASSERT(false);
				g_log->error("Cannot set priority: " + string(strerror(errno)));
			}

			//
			// Since 2.6.36, the previous code is not enough since
			// the kernel will make the nice level of the process effective
			// only within the process group, which is useless.
			// I found out the following hack by looking in the kernel source
			//
			ofstream autogroup_file("/proc/" + NumberFormatter::format(getpid()) + "/autogroup", std::ofstream::out);
			if(autogroup_file.is_open())
			{
				autogroup_file << AGENT_PRIORITY;
				if(autogroup_file.fail())
				{
					g_log->warning("Cannot set the autogroup priority");
				}

				autogroup_file.close();
			}
			else
			{
				g_log->warning("Cannot open the autogroup file");
			}
#endif
		}
#endif

		//
		// From now on we can get exceptions
		//
		try
		{
			m_configuration.m_machine_id = Environment::nodeId();

			//
			// Connect to the server
			//
			m_connection_manager.init(&m_configuration);

			sender_thread.m_thread.start(sender_thread);
			receiver_thread.m_thread.start(receiver_thread);

			//
			// Attach our transmit callback to the analyzer
			//
			m_inspector.set_analyzer_callback(&sinsp_handler);

			//
			// Plug the sinsp logger into our one
			//
			m_inspector.set_log_callback(g_logger_callback);
			if(!m_configuration.m_metrics_dir.empty())
			{
				//
				// Create the metrics directory if it doesn't exist
				//
				File md(m_configuration.m_metrics_dir);
				md.createDirectories();
				m_inspector.get_configuration()->set_emit_metrics_to_file(true);
				m_inspector.get_configuration()->set_metrics_directory(m_configuration.m_metrics_dir);
			}
			else
			{
				g_log->information("metricsfile.location not specified, metrics won't be saved to disk.");
			}

			//
			// The machine id is the MAC address of the first physical adapter
			//
			m_inspector.get_configuration()->set_machine_id(m_configuration.m_machine_id);

			//
			// The customer id is currently specified by the user
			//
			if(m_configuration.m_customer_id.empty())
			{
				g_log->error("customerid not specified.");
			}

			m_inspector.get_configuration()->set_customer_id(m_configuration.m_customer_id);

			//
			// Configure compression in the protocol
			//
			m_inspector.get_configuration()->set_compress_metrics(m_configuration.m_compression_enabled);

			//
			// Configure connection aggregation
			//
			m_inspector.get_configuration()->set_aggregate_connections_in_proto(!m_configuration.m_emit_full_connections);

			//
			// Start the capture with sinsp
			//
			g_log->information("Opening the capture source");
			if(m_filename != "")
			{
				m_inspector.open(m_filename);
			}
			else
			{
				m_inspector.open("");
			}

			aws_metadata metadata;
			if(m_configuration.get_aws_metadata(&metadata))
			{
				sinsp_ipv4_ifinfo aws_interface(metadata.m_public_ipv4, metadata.m_public_ipv4, metadata.m_public_ipv4, "aws");
				m_inspector.import_ipv4_interface(aws_interface);
			}

			if(m_configuration.m_dropping_mode)
			{
				g_log->information("Enabling dropping mode");
				m_inspector.start_dropping_mode();
			}

			if(m_writefile != "")
			{
				m_inspector.start_dump(m_writefile);
				dragent_configuration::m_dump_enabled = true;
			}

			//
			// Start consuming the captured events
			//
			do_inspect();
		}
		catch(sinsp_exception& e)
		{
			g_log->error(e.what());
			return Application::EXIT_SOFTWARE;
		}
		catch(Poco::Exception& e)
		{
			g_log->error(e.displayText());
			return Application::EXIT_SOFTWARE;
		}
		catch(...)
		{
			g_log->error("Application::EXIT_SOFTWARE\n");
			return Application::EXIT_SOFTWARE;
		}

		if(dragent_error_handler::m_exception)
		{
			g_log->error("Application::EXIT_SOFTWARE\n");
			return Application::EXIT_SOFTWARE;
		}
		else
		{
			g_log->information("Application::EXIT_OK\n");
			return Application::EXIT_OK;
		}
	}
	
private:
	bool m_help_requested;
	sinsp m_inspector;
	string m_filename;
	uint64_t m_evtcnt;
	string m_writefile;
	string m_pidfile;
	dragent_configuration m_configuration;
};


int main(int argc, char** argv)
{
	dragent_app app;
	return app.run(argc, argv);
}
