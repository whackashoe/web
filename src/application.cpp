#include <gud/application.hpp>

using namespace gud;

application::application(int argc, char * argv[])
	: args_(argv, argv + argc)
	, server_socket_(-1)
{
	bootup();
}

std::string application::rfc1123_datetime(time_t time)
{
	struct tm * timeinfo;
    char buffer [80];

    timeinfo = gmtime ( &time );
    strftime (buffer,80,"%a, %d %b %Y %H:%M:%S GMT",timeinfo);

    return buffer;
}

void application::bootup()
{
	gud::log::set_pattern(gud::config::get("app.log.format"));
	{
		std::string log_level = gud::config::get("app.log.level");

		if(log_level == "trace") {
			gud::log::set_level(spdlog::level::level_enum::trace);
		} else if(log_level == "debug") {
	    	gud::log::set_level(spdlog::level::level_enum::debug);
	    } else if(log_level == "info") {
	    	gud::log::set_level(spdlog::level::level_enum::info);
	    } else if(log_level == "notice") {
	    	gud::log::set_level(spdlog::level::level_enum::notice);
	    } else if(log_level == "warn") {
	    	gud::log::set_level(spdlog::level::level_enum::warn);
	    } else if(log_level == "err") {
	    	gud::log::set_level(spdlog::level::level_enum::err);
	    } else if(log_level == "critical") {
	    	gud::log::set_level(spdlog::level::level_enum::critical);
	    } else if(log_level == "alert") {
	    	gud::log::set_level(spdlog::level::level_enum::alert);
	    } else if(log_level == "emerg") {
	    	gud::log::set_level(spdlog::level::level_enum::emerg);
	    } else if(log_level == "off") {
	    	gud::log::set_level(spdlog::level::level_enum::off);
	    }
	}

	bool db_connect = gud::config::get("app.db.connect");
	if(db_connect) {
		const std::string host = gud::config::get("app.db.host");
		const int port = gud::config::get("app.db.port");
		const std::string username = gud::config::get("app.db.username");
		const std::string password = gud::config::get("app.db.password");
		const std::string database = gud::config::get("app.db.database");

		gud::db::connect(host, port, database, username, password);
	}
}

void application::mount_route(request::http_method method, std::string const & path, view_function_t view)
{
	view_map_t::iterator mount = views_.find(path);
	if (mount == views_.end()) {
		// Found no views for specified path.
		method_map_t methods;
		methods.insert(std::make_pair(method, view));
		views_.insert(std::make_pair(path, methods));

		return;
	}

	// Add new view.
	std::pair<method_map_t::iterator, bool> route = mount->second.insert(std::make_pair(method, view));
	if (!route.second) {
		throw std::logic_error("View already mounted at specified path (" + path + ").");
	}
}

void application::options(std::string const & path, view_function_t view)
{
	mount_route(request::http_method::OPTIONS, path, view);
}

void application::get(std::string const & path, view_function_t view)
{
	mount_route(request::http_method::HEAD, path, view);
	mount_route(request::http_method::GET, path, view);
}

void application::put(std::string const & path, view_function_t view)
{
	mount_route(request::http_method::PUT, path, view);
}

void application::patch(std::string const & path, view_function_t view)
{
	mount_route(request::http_method::PATCH, path, view);
}

void application::post(std::string const & path, view_function_t view)
{
	mount_route(request::http_method::POST, path, view);
}

void application::delete_(std::string const & path, view_function_t view)
{
	mount_route(request::http_method::DELETE, path, view);
}

void application::all(std::string const & path, view_function_t view)
{
	mount_route(request::http_method::WILDCARD, path, view);
}

application::view_function_t application::get_route(request::http_method method, std::string const & path)
{
	view_map_t::iterator mount = views_.find(path);
	if (mount == views_.end()) {
		return view_function_t(); // Path not found.
	}

	method_map_t::iterator route = mount->second.find(method);
	if (route == mount->second.end()) {
		route = mount->second.find(request::http_method::WILDCARD);

		if (route == mount->second.end()) {
			return view_function_t(); // Method not supported?
		}
	}

	return route->second;
}

std::string application::process(request & req, response & res) throw()
{
	unsigned int result_code = res.status_code();
	view_function_t view = get_route(req.method(), req.path());
	std::string response; // Site response.

	try {
		// Check if specified view exists.
		// If not, throw "404" - view does not exists.
		if (!view) {
			throw http_error(404);
		}

		// Run view.
		view(req, res);

		// Generated response.
		response = res.stream().str();
	} catch (gud::http_error const & e) {
		// Change HTTP result.
		result_code = e.error_code();

		// Generated response
		// (before the exception was raised)
		response = res.stream().str();
	} catch (std::exception const & e) {
		// We know what does this error (could) mean.
		result_code = 500;

		// Exception description is our response.
		response = e.what();
	} catch (...) {
		// We do not have idea what this error means.
		result_code = 500;
	}

	response += "\r\n";

	// Construct a valid HTTP response.
	std::stringstream output;
	output << "HTTP/1.1 " << result_code << " " << response::status_codes[result_code] << "\r\n";

	// Add all stored headers
	for(auto & i : res.headers()) {
		output << i.first << ": " << i.second << "\r\n";
	}

	// And custom "always" headers
	output << "Date: " << rfc1123_datetime(std::time(NULL)) << "\r\n";
	output << "Content-Length: " << response.length() << "\r\n";

	// Split to separate from body
	output << "\r\n";

	// Head requires no body
	if(req.method() != request::http_method::HEAD) {
		output << response;
	}

	return output.str();
}

std::vector<std::string> const & application::args() const
{
	return args_;
}

application::view_map_t const & application::routes() const
{
	return views_;
}

void application::listen(unsigned short const port, const char * address)
{
	server_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket_ < 0) {
		__throw_system_exception();
	}

	struct sockaddr_in serv_addr = {0};
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);

	int value = 1;

	if (::setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(serv_addr)) < 0) {
		__throw_system_exception();
	}
	if (::bind(server_socket_, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
		__throw_system_exception();
	}
	if (::listen(server_socket_, 1) < 0) {
		__throw_system_exception();
	}

	log::info("gud server listening on 127.0.0.1:{}", gud::config::get("server.port"));

	while (true) {
		struct sockaddr_in client_addr = {0};
		socklen_t client_len = sizeof(client_addr);
		const int client_socket = ::accept(server_socket_, (struct sockaddr *) &client_addr, &client_len);

		if (client_socket < 0) {
			__throw_system_exception();
		}

		// New client connected
		std::vector<char> raw_request(65535); // Need more?
		unsigned int pos = 0;

		while (pos < raw_request.size()) {
			const int n = ::read(client_socket, &raw_request[pos], raw_request.size() - pos);

			if (n < 0) {
				// Connection reset
				break;
			} else if (n == 0) {
				// Client disconnected
				break;
			}
			// Find headers
			pos += n;
			for (unsigned int i = 3; i < pos; i++) {
				if ((raw_request[i - 3] == '\r') &&
					(raw_request[i - 2] == '\n') &&
					(raw_request[i - 1] == '\r') &&
					(raw_request[i]     == '\n')
				) {
					// Found headers.
					std::string headers(raw_request.begin(), raw_request.end());
					request req(headers); // can throw

					response res;
					std::string const & data = process(req, res);

					// Flush raw response (with headers) to client.
					std::vector<char> buf(data.begin(), data.end());

					unsigned int pos = 0;
					while (pos < buf.size()) {
						int n = ::send(client_socket, &buf[pos], buf.size() - pos, 0);

						if (n < 0) {
							gud::log::info("Unable to write data to socket: {}", strerror(errno));
							break;
						}

						pos += n;
					}
				}
			}
		}

		::close(client_socket);
	}
}