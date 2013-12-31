/**
 * \file schat.cpp
 * \author Julien Kauffmann <julien.kauffmann@freelan.org>
 * \brief A simple chat client.
 */

#include <fscp/fscp.hpp>

#include <cryptoplus/cryptoplus.hpp>
#include <cryptoplus/error/error_strings.hpp>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <csignal>

static boost::function<void ()> stop_function = 0;
static boost::mutex output_mutex;

using boost::mutex;

static void signal_handler(int code)
{
	switch (code)
	{
		case SIGTERM:
		case SIGINT:
		case SIGABRT:
			if (stop_function)
			{
				std::cerr << "Signal caught: stopping..." << std::endl;

				stop_function();
				stop_function = 0;
			}
			break;
		default:
			break;
	}
}

static bool register_signal_handlers()
{
	if (signal(SIGTERM, signal_handler) == SIG_ERR)
	{
		std::cerr << "Failed to catch SIGTERM signals." << std::endl;
		return false;
	}

	if (signal(SIGINT, signal_handler) == SIG_ERR)
	{
		std::cerr << "Failed to catch SIGINT signals." << std::endl;
		return false;
	}

	if (signal(SIGABRT, signal_handler) == SIG_ERR)
	{
		std::cerr << "Failed to catch SIGABRT signals." << std::endl;
		return false;
	}

	return true;
}

static void simple_handler(const std::string& msg, const boost::system::error_code& ec)
{
	mutex::scoped_lock lock(output_mutex);

	std::cout << msg << ": " << ec.message() << std::endl;
}

static void multiple_handler(const std::map<fscp::server::ep_type, boost::system::error_code>& results)
{
	mutex::scoped_lock lock(output_mutex);

	for (std::map<fscp::server::ep_type, boost::system::error_code>::const_iterator result = results.begin(); result != results.end(); ++result)
	{
		std::cout << result->first << ": " << result->second.message() << std::endl;
	}
}

static bool on_hello(fscp::server& server, const fscp::server::ep_type& sender, bool default_accept)
{
	mutex::scoped_lock lock(output_mutex);

	std::cout << "Received HELLO request from " << sender << std::endl;

	server.async_introduce_to(sender, boost::bind(&simple_handler, "async_introduce_to()", _1));

	return default_accept;
}

static void on_hello_response(fscp::server& server, const fscp::server::ep_type& sender, const boost::system::error_code& ec, const boost::posix_time::time_duration& duration)
{
	mutex::scoped_lock lock(output_mutex);

	if (ec)
	{
		std::cout << "Received no HELLO response from " << sender << " after " << duration << ": " << ec.message() << std::endl;
	}
	else
	{
		std::cout << "Received HELLO response from " << sender << " after " << duration << ": " << ec.message() << std::endl;

		server.async_introduce_to(sender, boost::bind(&simple_handler, "async_introduce_to()", _1));

		std::cout << "Sending a presentation message to " << sender << std::endl;
	}
}

static bool on_presentation(fscp::server& server, const fscp::server::ep_type& sender, fscp::server::cert_type sig_cert, fscp::server::cert_type /*enc_cert*/, bool is_new)
{
	mutex::scoped_lock lock(output_mutex);

	std::cout << "Received PRESENTATION from " << sender << " (" << sig_cert.subject().oneline() << ") - " << (is_new ? "new" : "existing") << std::endl;

	server.async_request_session(sender, boost::bind(&simple_handler, "async_request_session()", _1));

	return true;
}

static bool on_session_request(const fscp::server::ep_type& sender, const fscp::cipher_algorithm_list_type&, bool default_accept)
{
	mutex::scoped_lock lock(output_mutex);

	std::cout << "Received SESSION_REQUEST from " << sender << std::endl;

	return default_accept;
}

static bool on_session(const fscp::server::ep_type& sender, fscp::cipher_algorithm_type, bool default_accept)
{
	mutex::scoped_lock lock(output_mutex);

	std::cout << "Received SESSION from " << sender << std::endl;

	return default_accept;
}

static void on_session_failed(const fscp::server::ep_type& host, bool is_new, const fscp::algorithm_info_type& local, const fscp::algorithm_info_type& remote)
{
	mutex::scoped_lock lock(output_mutex);

	std::cout << "Session failed with " << host << std::endl;
	std::cout << "New session: " << is_new << std::endl;
	std::cout << "Local algorithms: " << local << std::endl;
	std::cout << "Remote algorithms: " << remote << std::endl;
}

static void on_session_established(const fscp::server::ep_type& host, bool is_new, const fscp::algorithm_info_type& local, const fscp::algorithm_info_type& remote)
{
	mutex::scoped_lock lock(output_mutex);

	std::cout << "Session established with " << host << std::endl;
	std::cout << "New session: " << is_new << std::endl;
	std::cout << "Local algorithms: " << local << std::endl;
	std::cout << "Remote algorithms: " << remote << std::endl;
}

static void on_session_lost(const fscp::server::ep_type& host)
{
	mutex::scoped_lock lock(output_mutex);

	std::cout << "Session lost with " << host << std::endl;
}

static void on_data(const fscp::server::ep_type& sender, fscp::channel_number_type channel_number, boost::asio::const_buffer data)
{
	try
	{
		mutex::scoped_lock lock(output_mutex);

		std::cout << sender << " (" << static_cast<int>(channel_number) << "): " << std::string(boost::asio::buffer_cast<const char*>(data), boost::asio::buffer_size(data)) << std::endl;
	}
	catch (std::exception&)
	{
	}
}

void handle_read_line(fscp::server& server, std::string line)
{
	if (line[0] == '!')
	{
		std::istringstream iss(line.substr(1));

		std::string command;

		if (iss >> command)
		{
			if (command == "connect")
			{
				std::string host, port;

				iss >> host >> port;

				boost::asio::ip::udp::resolver resolver(server.get_io_service());
				boost::asio::ip::udp::resolver::query query(host, port);

				try
				{
					boost::asio::ip::udp::endpoint ep = *resolver.resolve(query);

					server.async_greet(ep, boost::bind(&on_hello_response, boost::ref(server), ep, _1, _2));

					std::cout << "Contacting " << ep << "..." << std::endl;
				}
				catch (std::exception& ex)
				{
					std::cerr << "Unable to resolve the specified host/port: " << ex.what() << std::endl;
				}
			} else if (command == "quit" || command == "exit")
			{
				server.close();
			}
		}
	} else
	{
		server.async_send_data_to_all(fscp::CHANNEL_NUMBER_0, boost::asio::buffer(line), multiple_handler);
	}
}

#ifdef BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR

void handle_read_input(fscp::server& server, boost::asio::posix::stream_descriptor& input, const boost::system::error_code& ec, boost::asio::streambuf& input_buffer, size_t length)
{
	if (!ec)
	{
		std::string line(length - 1, '\0');

		input_buffer.sgetn(&line[0], length - 1);
		input_buffer.consume(1);

		handle_read_line(server, line);

		boost::asio::async_read_until(input, input_buffer, '\n', boost::bind(&handle_read_input, boost::ref(server), boost::ref(input), boost::asio::placeholders::error, boost::ref(input_buffer), boost::asio::placeholders::bytes_transferred));
	} else
	{
		server.close();
	}
}

#endif

int main(int argc, char** argv)
{
	register_signal_handlers();

	try
	{
		if (argc != 7)
		{
			std::cerr << "Usage: schat <certificate> <private_key> <listen_host> <listen_port> <calg> <mdalg>" << std::endl;

			return EXIT_FAILURE;
		}

		const std::string certificate_filename(argv[1]);
		const std::string private_key_filename(argv[2]);
		const std::string listen_host(argv[3]);
		const std::string listen_port(argv[4]);
		const fscp::cipher_algorithm_type calg = boost::lexical_cast<fscp::cipher_algorithm_type>(argv[5]);

		cryptoplus::crypto_initializer crypto_initializer;
		cryptoplus::algorithms_initializer algorithms_initializer;
		cryptoplus::error::error_strings_initializer error_strings_initializer;

		boost::asio::io_service io_service;

		boost::asio::ip::udp::resolver listen_resolver(io_service);
		boost::asio::ip::udp::resolver::query listen_query(listen_host, listen_port);
		boost::asio::ip::udp::endpoint listen_ep = *listen_resolver.resolve(listen_query);

		cryptoplus::x509::certificate certificate = cryptoplus::x509::certificate::from_certificate(cryptoplus::file::open(certificate_filename, "r"));
		cryptoplus::pkey::pkey private_key = cryptoplus::pkey::pkey::from_private_key(cryptoplus::file::open(private_key_filename, "r"));

		const std::string local_name = certificate.subject().find(NID_commonName)->data().str();

		fscp::server server(io_service, fscp::identity_store(certificate, private_key));

		const fscp::cipher_algorithm_list_type calg_capabilities(1, calg);

		server.set_cipher_capabilities(calg_capabilities);

		server.open(listen_ep);

		server.set_hello_message_received_callback(boost::bind(&on_hello, boost::ref(server), _1, _2));
		server.set_presentation_message_received_callback(boost::bind(&on_presentation, boost::ref(server), _1, _2, _3, _4));
		server.set_session_request_message_received_callback(&on_session_request);
		server.set_session_message_received_callback(&on_session);
		server.set_session_failed_callback(&on_session_failed);
		server.set_session_established_callback(&on_session_established);
		server.set_session_lost_callback(&on_session_lost);
		server.set_data_received_callback(&on_data);

		std::cout << "Chat started. Type !quit to exit." << std::endl;

		boost::thread_group threads;

		const unsigned int THREAD_COUNT = boost::thread::hardware_concurrency();

		std::cout << "Starting client with " << THREAD_COUNT << " thread(s)." << std::endl;

		for (std::size_t i = 0; i < THREAD_COUNT; ++i)
		{
			threads.create_thread(boost::bind(&boost::asio::io_service::run, &io_service));
		}

#ifdef BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR
		boost::asio::streambuf input_buffer(512);
		boost::asio::posix::stream_descriptor input(io_service, ::dup(STDIN_FILENO));
		boost::asio::async_read_until(input, input_buffer, '\n', boost::bind(&handle_read_input, boost::ref(server), boost::ref(input), boost::asio::placeholders::error, boost::ref(input_buffer), boost::asio::placeholders::bytes_transferred));

		stop_function = boost::bind(&boost::asio::posix::stream_descriptor::close, &input);

#else

		stop_function = boost::bind(&fscp::server::close, &server);

		std::cout << "No POSIX stream descriptors available. Press Ctrl+C twice to exit." << std::endl;

		char line[512] = {};

		while (std::cin.getline(line, sizeof(line)))
		{
			server.get_io_service().post(boost::bind(&handle_read_line, boost::ref(server), std::string(line)));
		}	

#endif

		threads.join_all();

		stop_function = 0;

		std::cout << "Chat closing..." << std::endl;
	}
	catch (std::exception& ex)
	{
		std::cerr << "Error: " << ex.what() << std::endl;

		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
